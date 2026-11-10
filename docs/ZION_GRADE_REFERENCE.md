# Zion Grade — Implementation Reference

**Repository:** `unplugged-VideoRenderer` (MPC Video Renderer, Zion fork)  
**Status:** Current implementation, 2026-11-04  
**Audience:** future maintainers, model/training work, and technical marketing  
**Supersedes for current behaviour:** `ZION_ML_GRADE.md` (2026-09-09 snapshot), `Zion-ML-Integration-Plan.md` (port plan)

---

## TL;DR

The Zion Grade is a **learned correction layer**, not another tone curve.

- A fixed tone mapper (Unplugged/Hable or GT7 Adapt) produces the base HDR→SDR image. That part is still deterministic mathematics.
- Three XGBoost regressors read **88 source-frame features** and predict how a Dolby Vision colorist shaped **shadow toe, mid-tone gamma and highlight roll-off** for that scene.
- The predictions are applied as a single unified curve on the **I (intensity) channel of ICtCp**, so luminance changes do not drag hue or saturation with them.
- Two ONNX segmentation models identify **sky** and **people**, so the grade can relax the highlight guard inside sky and protect skin from chroma/clarity push.
- On GT7/GT7 Adapt, a deterministic **band controller** closes the loop: it solves shadow slope, gamma and roll-off so the *delivered* p25/p50/p99 land on house targets, using the ML prediction as the prior.
- Runtime is **metadata-free for the models**. Dolby Vision L2 colorist trims are used offline as training labels, not read at playback. That is the transferable idea: learn the mapping from image statistics to colorist decisions.

The short marketing-safe claim:

> Most video processors apply a fixed curve — BT.2390, a spline, or a custom transfer function. Zion learns the correction a colorist made from the image itself, predicts it per scene, and then a closed-loop controller lands the delivered picture on the display targets.

Do **not** claim the base tone mapping is ML, that the models reconstruct the original DV metadata, or that the XGBoost models are neural networks.

---

## 1. What is ML and what is not

| Layer | Mechanism | ML? |
|---|---|---|
| Base tone mapping (Unplugged/Hable, GT7, GT7 Adapt) | Deterministic transfer function + display target | No |
| Shadow / gamma / highlight prediction | Three XGBoost additive-tree regressors | **Yes** |
| Sky segmentation | YOLO26 semantic segmentation (ONNX + DirectML) | **Yes** |
| Person segmentation | YOLO26 instance segmentation (ONNX + DirectML) | **Yes** |
| Skin probability LUT | Pre-trained 2D LUT (Cr/Cb → P(skin)) | Fixed LUT, not live inference |
| Chroma boost | Deterministic curve; skin LUT/person mask gate | No (gated, not learned) |
| Micro-contrast | Deterministic multiplicative clarity; texture gate + person mask | No |
| Band controller | Secant solve over the grade's closed-form chain | No (closed-loop control) |
| Taste dials (exposure / shadow detail / punch) | User operators, applied last | No |

The important distinction for messaging: **the automatic look is learned; the transfer function it rides on is not.** The models predict corrections; the controller and shaders execute them.

---

## 2. System map

```
Source frame (PQ HDR or SDR)
  │
  ├─ base tone mapping (Unplugged/Hable or GT7 / GT7 Adapt)
  │
  ├─ 256×144 luma readback  ──►  88-feature vector
  │                                  │
  │                                  ├─► xgb_shadow_recovery   → shadow scalar
  │                                  ├─► xgb_gamma_model       → gamma scalar
  │                                  └─► xgb_highlight_rolloff → roll-off scalar
  │                                  │
  │                                  ▼
  │                            MLParams_t cbuffer (368 bytes)
  │                                  │
  ├─ segmentation @ ~10 fps ──► sky/person R8G8 mask (content UV)
  │                                  │
  ▼                                  ▼
  ps_ml_highlight (passthrough)
  ps_ml_gamma     (shadow slope + toe + gamma + roll-off + sky leg)
  ps_ml_shadow    (micro-contrast, person-gated)
  ps_ml_chroma    (chroma boost + highlight desat, skin-gated)
  ps_ml_taste     (user shape dials — last automatic word)
  ps_ml_bars      (letterbox repaint)
  custom post-scale (NLS / deband / sharpen)
  post-warp MC (when NLS moves the geometry)
  encode → calibration → dither → display
```

Two measurements run alongside:

- **Anchor IIR**: p99.5 scene peak + mean nits, filtered and scene-cut-snapped. Feeds the base curve normaliser and the controller.
- **Delivered stats**: downsampled post-correction and post-grade frames, reduced to [DELIVERED] lines. This answers “what did we actually deliver”, which the source-referenced models cannot see.

---

## 3. The feature contract — 88 slots

The models were trained on a fixed 88-entry vector. **Do not add, remove or reorder slots**; the models are additive trees walking splits by index, so a shifted vector silently changes every prediction.

| Slots | Content |
|---|---|
| `[0]` | `maxscl` — PQ peak of the frame |
| `[1]` | PQ average |
| `[2]` | fraction of pixels above 0.5 PQ |
| `[3..8]` | p25, p50, p75, p90, p95, p99 |
| `[9..26]` | 3×3 zone means + maxima |
| `[27..76]` | 5×5 zone means + maxima |
| `[77..84]` | 8 knots of the libplacebo spline tone-map LUT (`output_max = 0.5444`) |
| `[85]` | top letterbox bar fraction |
| `[86]` | bottom letterbox bar fraction |
| `[87]` | compression strength (`0.5444 / max(maxscl, floor)`) |

Implementation: `Source/ml_features.cpp`, contract in `Source/ml_features.h`.

Key properties:

- The histogram is 65,536 bins; percentiles use numpy-style linear interpolation.
- Zone statistics use a **summed area table (SAT)** with a one-cell margin, so 3×3/5×5 queries are O(1).
- All luma statistics are in PQ code units, 0..1. SDR readbacks are remapped into source-PQ units before stats run (`MLLumaDomain::SDRGamma22`), so the model sees the domain it was trained on.
- Letterbox/pillarbox geometry matters: the 3×3/5×5 zones and bar slots are part of the training distribution.

### Separate statistics (not feature slots)

`BuildMLFeatures` also emits, deliberately outside the 88-vector:

- `outPeakP995` — p99.5 peak over the content rows; the anchor statistic. p99.995 was tried and discarded: on a 256×144 readback it drops the top ~2 pixels, letting a speck set the anchor.
- `outActiveFrac` — fraction of 16×16 cells containing a non-black pixel; the scene-change bias.
- `outMeanNits` — mean luminance in **nits** over content rows. The PQ-coded mean (`features[1]`) decodes to a fraction of a nit and is useless for exposure.
- `outQuantiles` — 33 quantiles for the Dynamic & Auto curve solve.
- `BuildRenderedLumaStats` — delivered-frame percentiles, crush fraction and shadow mean; the closed-loop instrument.

This separation is an invariant: the model contract stays byte-identical while new instruments are added around it.

---

## 4. The three models

Real metadata read from the shipped `.plxgb` files:

| Model | File | Features | Trees | Base score | Learning rate | Size |
|---|---|---:|---:|---:|---:|---:|
| Gamma | `xgb_gamma_model.plxgb` | 88 | 500 | 1.2703 | 0.05 | 1,316,704 B |
| Shadow toe | `xgb_shadow_recovery.plxgb` | 88 | 600 | 0.0444 | 0.05 | 1,560,380 B |
| Highlight roll-off | `xgb_highlight_rolloff.plxgb` | 88 | 600 | 0.1245 | 0.05 | 1,599,232 B |

Total: **1,700 additive trees, ~4.48 MB** of model data, 88 features each.

### Training targets — the “colorist intent” contract

| Model | Target | Output clamp |
|---|---|---|
| Gamma | `2048 / trim_2081_power` from Dolby Vision L2 | `[0.5, 1.5]` |
| Shadow toe | `(lift_delta × 0.60 + shadow_crush × 0.40) × 0.40` — L2 offset pedestal + native shadow crush | `[0.0, 0.5]` current code |
| Highlight roll-off | mean gold curve at the top two knots − mean libplacebo spline baseline | `[-0.3, 0.4]` |

Interpretation:

- Gamma > 1 means the colorist added mid-tone contrast; < 1 means a softer mid-tone.
- Shadow toe > 0 means the colorist lifted the dark end; it is deliberately scaled conservatively.
- Highlight roll-off > 0 means the colorist expanded the shoulder; < 0 means a darker, more filmic roll-off.

**The models are metadata-free at runtime.** The models do not read MaxCLL, L2 trims or Dolby Vision RPU data. They infer the correction from the 88 image features. Dolby Vision L2 is an offline training label only — that is the whole point of the approach. (The base tone mapper may still use display/content metadata; this statement is about the Zion models.)

Training design targets a large Dolby Vision Profile 5 corpus (the integration plan references a ~150K-scene corpus). The training scripts live outside this repository (`libplacebo/ml/`); the shipped artifacts are the `.plxgb` files. The training data and model cards are planned for future release; until then, treat the exact corpus counts as design intent, not a released fact.

### Model format and inference

`Source/ml_model.cpp` implements a dependency-free reader for the `.plxgb` format:

- Magic `0x47584c50` (`PLXG`), version 1.
- Header: feature count, tree count, reserved, base score, learning rate.
- Per tree: node count, then nodes `(left, right, feature, threshold, default_left, split_type)` and a weight per node.
- Inference is an additive leaf walk. The exported leaf weights already contain the effective contribution; the loader deliberately **does not apply learning rate again**.
- Models load once at filter construction (`GetMLModelPath` → same directory as the `.ax`). Missing models are logged and disable the relevant prediction; the filter does not lazy-load on the render thread.

Inference is CPU-side and designed to be sub-millisecond per frame (integration-plan target <0.1 ms). The expensive work is the 256×144 readback and segmentation, not the tree walk.

---

## 5. Per-frame prediction and temporal behaviour

`CDX11VideoProcessor::SetMLParams()` (`Source/DX11VideoProcessor.cpp`) is the central packer.

### Modes

| Mode | Behaviour |
|---|---|
| **Auto** | Models predict shadow/gamma/highlight per frame; gamma is IIR-smoothed; scene cuts snap the filter |
| **Manual** | User values drive the same shader parameters; no model dependency |
| **Off** | Neutral values: shadow 0, gamma 1, roll-off 0 |

The master `Enable ML grade` switch and per-section enables gate the individual models. If the gamma model is missing, Auto is not considered ready.

### Smoothing

- Gamma: IIR `0.10 × new + 0.90 × held`, reset on a scene cut (`m_bSceneCut`) or a large `features[0]` jump.
- Texture-energy gate for micro-contrast: same IIR/snap pattern.
- The controller has its own input and output IIR (see §6).

The smoothing is noise control, not convergence. A camera cut is a frame replacement; it is explicitly exempted so the picture does not arrive late at every cut.

### User offsets on top of the models

- **Gamma top-up** — a signed offset added to the model's exponent in Auto. Additive, not multiplicative: a multiplier cannot lift a scene the model judged neutral because 1.0 is a fixed point. It is passed to the shader as a separate `MLGammaTopUp` field so the gamma pass can hold it back inside skin using the trained skin LUT.
- **Highlight shoulder override** — a signed offset added to the model's roll-off in Auto, clamped ±0.20.
- **Shadow dehaze** — scales **down** the model's shadow-toe lift (`toe × (1 − dehaze)`), for the lifted-black haze case.
- **Relative mid-tone target** (`fMLTargetMid`) — on GT7/GT7 Adapt, solves the gamma that lands the scene average at the requested encoded level, compensating for what the base curve and display target did. This is the delivery-referenced top-up; it is not a model output.

### Shadow actuators: slope, toe and pedestal

Three different things, often confused:

1. **Shadow toe** (`MLShadowToe`) — the model output. A gain proportional to `x`; it cannot lift a pixel at code 0.
2. **Shadow slope** (`MLShadowSlope`) — a power below the knee. `0` stays `0`, the knee stays fixed, and the code span containing dark detail is expanded. This is the controller's shadow actuator.
3. **Shadow pedestal** (`MLShadowPedestal`) — a true additive offset, applied as neutral light after the curve. It can lift a black pixel, but done naively it veils the picture; it is bounded to 8% and gated to content. The controller deliberately leaves it at zero and solves the slope instead.

---

## 6. The band controller — the closed loop on delivered luminance

**Where:** `SetMLParams()`, the “JOINT BAND CONTROLLER” block, plus `SolveGrade()` and `GradeChainDelivered()` in `Source/DX11VideoProcessor.cpp`.

### Why it exists

The XGBoost models measure the **source** and emit an absolute correction. They cannot see what the base tone curve, `refWhite`, display target or calibration did to the picture. A model prediction that was right for the training curve can therefore land the delivered mid-tone in the wrong place.

The controller inverts the actual grade chain and solves corrections so the delivered bands hit house targets.

### Gate

```cpp
m_bMLController && m_bMLFeaturesReady &&
(m_iHdrLocalToneMappingType == TONE_CURVE_GT7 ||
 m_iHdrLocalToneMappingType == TONE_CURVE_GT7_ADAPT) &&
m_iSDRDisplayNits > 0 && m_fCurvePeakNits > 10.0f
```

The controller is currently **GT7 / GT7 Adapt only**. It does not run on the Unplugged/Reference curve. The renderer struct default is `bMLController = false`; the shipped UI/preset path is expected to enable it. Verify the effective value from the `CTLSTATE on=` line in `ml_debug.csv` rather than assuming the UI default reached the renderer.

### Inputs

- `m_BandPq[3]` — scene-locked p25/p50/p99 in source PQ.
- `m_BandPeakPq` — scene-locked p99.5 peak in source PQ.
- Fallbacks: live `features[3/4/7]` and `m_MLPeakPq995` until the bands have a first reading.
- `m_fCtlWhiteNits` — house white (203 nits default; ~335 for the cloud-safe taste).
- `m_fEffRefWhite`, `m_fCurvePeakNits`, `m_iSDRDisplayNits`.

Scene-locked bands are essential. An earlier version solved on raw per-frame percentiles while the peak anchor was frozen; the free-running half was the one that set brightness, so a coat entering frame or snow drifting pumped the whole picture. The fix was to solve on the same locked statistics the anchor uses.

### Target law

```cpp
r     = min(1, nits / peakNits);          // position within this scene's range
level = clamp(203 / ctlWhiteNits, 0.5, 2);
wT    = clamp(level * pow(r, 0.75), 0, 1);
wT    = max(wT, wA);                       // lift only
```

`wA` is the anchor's delivered fraction of white; `wT` is the target. The `r^0.75` law lifts shadows in proportion to the scene's own contrast, so a night scene stays a night scene but its detail moves above the display's invisible floor. The lift-only clamp exists because an earlier absolute law spent its authority darkening bright scenes.

### Solve

- Three anchors (p25/p50/p99) → three targets.
- `SolveGrade()` runs secant steps on three actuators: `S` shadow slope, `G` gamma, `R` roll-off.
- Two passes, because the highlight knee is derived from gamma and gamma is being solved.
- The model's prediction is the **prior**: `baseG = gamma + gammaTopUp`, `baseR = rolloff`; the controller solves **corrections** on top (`ctlG - 1`, `ctlR`), not replacements. Replacing gamma outright deleted the model's lift on dark scenes, where all three anchors sit in the gamma stage's fade-blind zone and the solver returns ~1.0.

### Temporal behaviour

- Inputs: IIR 0.10; snap on a cut or a large relative jump.
- Outputs: IIR 0.10 with wider jump thresholds; a cut snaps all three actuators in one frame.
- The applied parameters are logged every 6 frames as `CTL` (anchors, targets, S/G/R, delivered values, refWhite, peak, SDR nits).

### What it deliberately does not do

- No highlight darkening: `wT >= wA` makes the controller lift-only. The curve's soft ceiling and the gamut cap own clipping.
- No shadow pedestal: it solves the toe slope, because an additive pedestal greys the whole picture instead of revealing detail.
- Not a replacement for the model: it corrects the model's delivery, and stands down the legacy shadow toe when active (`MLShadowToe = 0`).

---

## 7. Anchor IIR and scene exposure memory

The controller and the base curve share a scene anchor. The full derivation is in `ANCHOR_IIR.md`; the invariants:

| Event | Behaviour |
|---|---|
| Steady content, sub-1 dB drift | IIR low-pass (period 20, ~0.8 s at 24 fps); no visible re-normalisation |
| Real scene cut (>3 dB peak/mean jump) | Hysteresis mix reaches 1 → average and peak snap in one frame |
| Failed/saturated read (>4000 nits) | Plausibility gate skips; filter holds its last value |
| Letterbox / mostly-black frame | Active **cell** fraction biases delta to ~0 → no false cut |
| First frames | Negative peak → shader falls back to static MaxCLL until a reading exists |

Additional layers in current code:

- **Scene lock** — within a scene the anchor is held; it only drifts downward (slow brighten) once locked.
- **Scene exposure memory** — exposure is a property of a **scene**, not a shot. A matched reverse angle replaces the frame and moves every statistic without changing the light. The envelope is a **veto on adaptation**, never a filter on the applied value: an accepted change still snaps in one frame.
- **Shot history / cluster median** — rapid cutting is treated as matched coverage; such shots are represented by the median of the cluster, never by whichever angle is on screen.

The anchor statistic is **p99.5 over content rows**, not `features[0]` (a max). A max is set by one speck; a percentile is a statistic.

---

## 8. Sky and person segmentation

### Sky

- Model: `yolo26n-sem.onnx` — Ultralytics semantic segmentation, Cityscapes 19-class, **sky = class 10**, 640×640 input, uint8 argmax output.
- Runtime: ONNX Runtime + DirectML; CPU fallback; session loaded once; optional (missing model logs and disables).
- Input: 640×360 SDR-ized BGRA preview from `ps_ml_face_down`, gray-114 letterbox.
- Decode: crop the letterbox padding before uploading; pad rows must not reach the shader as a hard “not-sky” band.
- Mask: `R8_UNORM` 640×360, sampled on `t2.r`; packed with person into `R8G8` (R=sky, G=person).
- Cadence: ~10 fps (frame counter), mask persists between runs.
- Effects (all scale with `MLSkyStrength`, 0 = bit-identical):
  1. Relaxed highlight guard inside sky (wider highlight range survives).
  2. Softened highlight roll-off inside sky.
  3. Cloud-band lift in the upper-mid luma band.
- Current code: the sky mask steers the **tone curve only**. Micro-contrast is person-gated, not sky-upweighted. This changed in the `skyMC-decouple-v3` build; older docs that describe sky-upweighted MC are historical.

### Person

- Model: `yolo26n-seg_op21.onnx` — Ultralytics instance segmentation, COCO 80-class, **person = class 0**, 640×640 input; output0 `[1,300,38]` post-NMS detections, output1 `[1,32,160,160]` mask prototypes.
- The raw export is opset 22; the shipped file is re-stamped to **opset 21** for the bundled ORT 1.21.
- Decode: per instance `{cls==0, score>=0.25}`, `sigmoid(proto · coeff)`, resize, threshold >0.5, union, letterbox-crop to the content rect; 640×360 person map + boxes.
- Mask: `t2.g`.
- Effects:
  - Chroma skin LUT weight is gated to real people (`LUT-only-inside-people`), killing false positives on red/orange objects.
  - Micro-contrast damping on person/skin.
  - Skin tone-preservation (stage 5): source-anchored luma remap on person pixels; default off, 0 = bit-identical legacy.
- Cadence: same ~10 fps as sky; the mask keeps person protection alive even when sky effects are off.

### Mask sampling invariant

The mask is content-aligned. Every shader remaps:

```hlsl
float2 cuv = (input.Tex - float2(MLSkyUVOffX, MLSkyUVOffY))
           / max(float2(MLSkyUVScX, MLSkyUVScY), 0.001f);
const float inContent = all(cuv >= 0 && cuv <= 1) ? 1 : 0;
float S = mask.Sample(sampSky, saturate(cuv)).r * inContent;
```

The four UV fields are refreshed every frame from the content rect. Without the in-content gate, letterbox/pillarbox pixels read the clamped mask edge and bleed sky/person into the bars.

### Face detection

YuNet + ByteTrack is present but **disabled** at construction (`#if 0`). Person instance masks replace it; the face cbuffer fields remain zeroed for layout stability. Face-guided gamma protection is not active.

---

## 9. Shader stages and colour science

### Stage order (current)

1. `ps_ml_highlight` — passthrough (kept for step-count/layout stability).
2. `ps_ml_gamma` — unified curve: shadow slope → toe → gamma (sky-relaxed guard) → highlight roll-off → cloud-band lift; skin-gated top-up; skin tone-preservation.
3. `ps_ml_shadow` — micro-contrast (post-curve I_norm), person-gated skin damping.
4. `ps_ml_chroma` — Ct/Cp boost with skin protection; highlight desaturation window (SDR path).
5. `ps_ml_taste` — user shape dials, last automatic word.
6. `ps_ml_bars` — letterbox repaint.
7. Custom post-scale chain (NLS, deband, sharpen).
8. Post-warp micro-contrast when NLS moves the geometry.

### Colour invariant

All luminance operations happen in **ICtCp**, and the curve's move is turned into a per-pixel **linear gain on the whole colour** (`rgb_out = ictcp_to_linear(ictcp) * g`). Moving `I` while holding Ct/Cp absolute changes relative saturation; scaling the whole colour makes a curve move a luminance move and preserves hue. The same treatment is used in micro-contrast.

The skin LUT is sampled in gamma-2.2 Cr/Cb, the space it was trained in; the boost is applied in ICtCp Ct/Cp. Detection and application are deliberately in different spaces.

### Cbuffer

`MLParams_t` is **368 bytes** (92 floats, 23 float4 groups), asserted at compile time. Fields are appended as a tail; every `ps_ml_*` shader must declare a **prefix** of the layout, never a longer one. The cbuffer is refreshed once per frame from `m_lastMLParams`, including the four sky-UV fields.

---

## 10. Settings and modes

User-facing controls (full detail in `docs/settings-guide.md` in the main repo):

- **Zion → Tone Curve**: Enable ML grade; Band controller; Mode Auto/Manual/Off; Shadow, Shadow dehaze, Gamma, Gamma top-up, Mid-tone target, Highlight, Highlight shoulder.
- **Zion → Chroma**: Normal, Fire, Knee, Skin.
- **Zion → Micro Contrast**: Enable, Strength, Zone lo, Ratio cap, Skin protect.
- **Zion → Sky Seg**: Enable, Strength, Mask overlay.
- **Zion → Person Skin**: Enable, Strength, Mask overlay.
- **Zion → Debug**: HDR passthrough, Tone curve passthrough, Frame log, MC heat.

The engine/preset supplies the effective values; the `Settings_t` defaults in `IVideoRenderer.h` are not always the shipped UI defaults (for example `bMLController` defaults false there, while the shipping UI describes the band controller as on). Always verify the effective value in the log.

---

## 11. Runtime, files and deployment

### Model files

```
Source/onnxrt/xgb_gamma_model.plxgb         1,316,704 B
Source/onnxrt/xgb_shadow_recovery.plxgb     1,560,380 B
Source/onnxrt/xgb_highlight_rolloff.plxgb   1,599,232 B
Source/onnxrt/yolo26n-sem.onnx              sky segmentation
Source/onnxrt/yolo26n-seg_op21.onnx         person segmentation
Source/res/skin_chroma_lut.bin              96×96 skin probability LUT
```

All are resolved from the module directory (`GetMLModelDir()` / `GetMLModelPath()`), so they must ship next to the `.ax`. Models load deterministically at construction; there is no lazy load in `Render()` and no load/unload when toggling the grade.

### Per-frame resources

- `m_MLDownsample` / `m_MLStaging`: 256×144 `R16_UNORM` luma readback.
- `m_SegDownsample`: 640×360 BGRA preview for segmentation.
- `m_SkyMask`: `R8G8` sky/person mask at 640×360.
- `m_pMLParams`: 368-byte cbuffer.
- `m_SkinLUT`: 96×96 skin LUT texture.

### Budget

- Model inference: designed for sub-millisecond CPU time; 1,700 additive trees.
- Feature readback: 256×144 × 2 bytes ≈ 72 KB/frame, CPU reduction.
- Segmentation: ~10 fps, off the per-frame path; DirectML on GPU.
- The grade adds several full-screen passes; the existing pipeline already accounts for the ring steps, so toggling the grade does not change the step count.

---

## 12. Instrumentation and verification

`ml_debug.csv` (frame-log gated) is the primary instrument:

| Line | What it answers |
|---|---|
| `INIT_MODELS` | Which models loaded |
| `EXTRACT` / feature rows | What the 88 features were, domain, bars, peak |
| `MLPASS` | Step, rect, content UV, mask fracs |
| `GAMMA_DRAW` / `MC_DRAW` / `CHROMA_DRAW` / `TASTE_DRAW` | Which shader drew, which resources bound |
| `CTL` / `CTLSTATE` | Controller gate, anchors, targets, solved S/G/R, delivered values |
| `DELIVERED` | Post-correction and post-grade p01..p99, crush fraction, shadow mean |
| `SKY_REFRESH` / `SKYUV` | Segmentation cadence, mask UV remap |
| `MC_TRACE` / `MC_SUM` | Texture gate, raw/smoothed energy, applied strength |

Other instruments:

- `[CBPROBE]` GPU read-back proves the constant buffer the shader bound is the one that was updated.
- `_gt7probe/` contains offline curve/solve/AB probes; `taste_check.ps1` asserts neutral-dial identity.
- `test_ml_loader.cpp` / `test_ml_sky.cpp` exercise the model and segmentation wrappers.
- The Integration Plan's acceptance gate: features must match a Python XGBoost reference to ≤1e-4 before Auto is trusted.

### Invariants to preserve

1. The 88-feature vector is a training contract. Add new statistics **outside** it.
2. The renderer's feature domain must match the shader's input domain (`MLFeatureDomainIsPq()` vs `MLInputIsPq()`).
3. Every temporal filter must snap on a scene cut; a cut is not drift.
4. Every “0 = off” path must be bit-identical to the pre-feature pipeline.
5. `MLParams_t` may only grow at the tail; shaders mirror a prefix.
6. The controller's model gamma is a prior, not a replacement; solve base + correction.
7. Do not use a max where a percentile is required; do not use a PQ-code mean where nits are required.

---

## 13. Known limitations and open work

- **The base tone mapping is still mathematical.** The ML layer predicts corrections; it does not replace BT.2390/Hable/GT7. Any claim that the whole chain is “not a curve” is false.
- **The models approximate colorist intent; they do not reconstruct the original metadata.** Runtime is metadata-free; L2 is training labels only. Out-of-distribution sources (unknown grading styles, animation, SDR) can be mispredicted.
- **Training data/model cards are not released yet.** The corpus size and exact labels should not be quoted as released facts.
- **Sky/person segmentation is WIP.** ~10 fps cadence, per-title false positives/negatives, and no temporal mask tracking beyond persistence. Sky no longer feeds micro-contrast; that decoupling is current, older docs are stale.
- **The band controller is GT7-only and experimental.** It is gated to GT7/GT7 Adapt, has a renderer default that may differ from the UI, and its scene-locked inputs depend on the anchor state machine. Treat it as a closed-loop add-on, not a universal grade stage.
- **Native SDR** currently reaches the grade (the `IsPQ=1/0` path), but the models were trained on Dolby Vision/HDR colorist trims. SDR behaviour needs separate validation before it is marketed as equivalent.
- **No confidence output.** The models emit a scalar; there is no per-frame uncertainty, fallback classifier or “this looks out of distribution” signal.
- **Face-guided protection is not active.** YuNet/ByteTrack is disabled; person masks are the continuity layer.
- **Saturation gain is not modelled.** It is largely per-title taste (71.5% between-title variance), so the chroma curve uses fixed defaults.
- **Hunt/DoG legacy paths remain dormant.** They are retained for layout/experiment but are not part of the shipping picture.

---

## 14. Positioning guidance — accurate claims for marketing

### Safe, specific claims

- “Three XGBoost models learn how a colorist adjusted shadow, mid-tone and highlight, then predict those adjustments per scene from 88 image features.”
- “1,700 trees, 88 features, per frame; the model inference is metadata-free — the Dolby Vision colorist intent is learned offline, not read from the signal.”
- “The prediction is applied on the ICtCp intensity channel and carried back as a luminance gain, so hue and saturation do not drift with the curve.”
- “Sky and person segmentation protect the highlights and skin that a global curve cannot distinguish.”
- “On GT7, a closed-loop controller solves the delivered p25/p50/p99 against house targets, with the model prediction as the prior.”
- “This is a different mechanism from a fixed BT.2390, spline or custom transfer curve: those apply a chosen operator; Zion predicts the correction from the image.”

### Claims to avoid

- “Reconstructs the original Dolby Vision metadata.” It does not; it infers corrections from image statistics.
- “The whole pipeline is AI / no mathematical curve.” The base tone mapper is deterministic; the ML is the grade layer.
- “Neural network” for the XGBoost models. They are gradient-boosted additive trees. The segmentation models are neural; the regressors are not.
- “Trained on 150K scenes” unless the corpus/model card is released and verified. Say “trained on Dolby Vision colorist trims” and “the design targets a large DV P5 corpus.”
- “Works on any SDR source” without qualification; native-SDR validation is incomplete.
- “No one else has ever done this.” Prefer “we are not aware of a shipping consumer video processor that learns the colorist correction this way.” The mechanism is differentiated; absolute novelty claims invite challenge.

### The one-sentence story

> Fixed processors apply a curve; Zion learns the colorist's correction from the image, predicts it per scene, protects sky and skin with segmentation, and closes the loop on what the display actually receives.

---

## 15. File map

### C++

| File | Role |
|---|---|
| `Source/ml_model.h/.cpp` | `.plxgb` loader and additive-tree inference |
| `Source/ml_features.h/.cpp` | 88-feature contract, SAT/zone/percentiles, bars, delivered stats |
| `Source/DX11VideoProcessor.h/.cpp` | model loading, `SetMLParams`, feature extraction, controller, render passes, cbuffer |
| `Source/IVideoRenderer.h` | `Settings_t`, mode/curve enums, controls |
| `Source/sky_seg.h/.cpp` | sky ONNX wrapper |
| `Source/person_seg.h/.cpp` | person ONNX wrapper |
| `Source/face_detect.h/.cpp` | YuNet wrapper (disabled) |
| `Source/tone_spline.h` | spline knot helper for feature slots [77..84] |

### Shaders

| File | Role |
|---|---|
| `Shaders/d3d11/ps_ml_gamma.hlsl` | unified curve, sky leg, skin gate |
| `Shaders/d3d11/ps_ml_shadow.hlsl` | micro-contrast, person-gated |
| `Shaders/d3d11/ps_ml_highlight.hlsl` | passthrough (layout stability) |
| `Shaders/d3d11/ps_ml_chroma.hlsl` | chroma boost, highlight desat, skin |
| `Shaders/d3d11/ps_ml_taste.hlsl` | user shape dials |
| `Shaders/d3d11/ps_ml_bars.hlsl` | bar repaint |
| `Shaders/d3d11/ps_ml_luma.hlsl` | 256×144 feature downsample |
| `Shaders/d3d11/ps_ml_face_down.hlsl` | 640×360 seg preview |

### Model/data

| File | Role |
|---|---|
| `Source/onnxrt/xgb_*.plxgb` | three XGBoost regressors |
| `Source/onnxrt/yolo26n-sem.onnx` | sky segmentation |
| `Source/onnxrt/yolo26n-seg_op21.onnx` | person segmentation |
| `Source/res/skin_chroma_lut.bin` | 96×96 skin probability LUT |

### Related docs

- `ZION_ML_GRADE.md` — 2026-09-09 detailed snapshot; useful history, but predates the taste stage, the band controller and the sky/MC decoupling.
- `Zion-ML-Integration-Plan.md` — port contract and original 88-feature design.
- `Zion-ACTION-PLAN-2026-09-07.md` — P0–P4 plan; bars/segmentation/skin history.
- `ANCHOR_IIR.md` — anchor chain, IIR, scene cut, Hable integration.
- `TONE_MAP_HANDOFF.md` — tone-map measurements, display floor, Hable knee, top-up.
- `docs/settings-guide.md` (main repo) — user-facing controls and defaults.

---

## 16. Glossary

- **Base curve** — the deterministic HDR→SDR transfer function (Unplugged/Hable, GT7, GT7 Adapt).
- **Zion Grade** — the ML-driven correction layer after the base curve.
- **L2 / trim_2081** — Dolby Vision per-scene colorist metadata used as training labels.
- **88-feature vector** — the fixed model input contract.
- **p99.5** — scene peak anchor over content rows; robust to specks.
- **Band controller** — deterministic closed-loop solve of shadow slope/gamma/roll-off on GT7.
- **Sky leg** — relaxed guard + softened roll-off + cloud lift inside the sky mask.
- **Person gate** — LUT-only-inside-people skin protection.
- **Taste stage** — user shape dials, applied after the automatic grade.
- **Delivered stats** — measurements of the rendered frame, not the source.

---

*This document describes the implementation as of 2026-11-04. When the code and a design note disagree, the code is authoritative; update this reference rather than adding another overlapping design note.*