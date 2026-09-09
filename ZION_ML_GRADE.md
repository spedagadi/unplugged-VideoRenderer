# Zion ML Grade — Technical Reference

## Overview

Zion ML Grade is a real-time video enhancement pipeline for MPC Video Renderer (MPCVR) that applies scene-adaptive tone mapping and chroma tuning using machine learning models trained on Dolby Vision Profile 5 metadata. All luminance operations are performed in the **ICtCp perceptual color space** (BT.2100 / SMPTE ST 2084 PQ transfer), which preserves chrominance (Ct/Cp) when the intensity (I) channel is modified — eliminating the hue and saturation shifts inherent in RGB or YCbCr-domain grading.

The pipeline consists of four shader passes plus ancillary feature extraction, face detection, **sky semantic segmentation** (yolo26n-sem via ONNX Runtime + DirectML), and black bar repaint. Three XGBoost models predict per-scene tone curve parameters from 88 GPU-extracted features for the automatic tone curve; the sky mask additionally relaxes the highlight guard *inside* detected sky so bright sky and cloud detail survive tone compression.

## Architecture

### Color Space: ICtCp

All tone curve and chroma operations happen in ICtCp:
- **I** (Intensity): perceptually uniform luminance — the sole channel modified by the tone curve
- **Ct** (Chroma Tritan): blue-yellow chroma axis
- **Cp** (Chroma Protan): red-green chroma axis

Operating on I alone while leaving Ct/Cp untouched ensures no skin tone or hue shift from luminance adjustments. This is a fundamental advantage over YCbCr or RGB grading where brightness changes bleed into saturation.

The conversion path: PQ-encoded RGB → ST 2084 EOTF → linear nits → ICtCp (via LMS with the BT.2100 cross-talk matrix) → modify I → inverse → PQ RGB. SDR content (gamma 2.2) follows the same pipeline with a linearization step.

### Render Pipeline Order

```
Source frame (PQ or SDR)
  │
  ├── Feature extraction (256×144 luma downsample → 88 histogram features)
  ├── Face detection (480×288 BGRA8 → YuNet ONNX → ByteTrack)
  ├── Sky segmentation (640×360 SDR-ized preview → yolo26n-sem ONNX → DirectML,
  │                     throttled ~10 fps; R8 content-aligned mask → t2 in shaders)
  │
  ▼
  1. ps_ml_highlight.hlsl — Highlight rolloff (passthrough — merged into gamma)
  2. ps_ml_gamma.hlsl     — Unified tone curve (shadow toe + gamma + highlight rolloff)
                             + sky tone leg (guard relaxation + cloud-band lift)
  3. ps_ml_shadow.hlsl    — Micro-contrast clarity (post-tone-curve texture lift,
                             sky-upweighted inside the mask)
  4. ps_ml_chroma.hlsl    — Chroma vector tuning (Ct/Cp boost with skin protection)
  5. ps_ml_bars.hlsl      — Black bar repaint (letterbox haze fix)
```

All passes share a single constant buffer (`MLParams_t`, 304 bytes) updated once per frame. The debug passthrough mode (`MLDebugPassthrough`) binds a simple copy shader to all steps for bisecting shader vs plumbing issues, and the separate HDR/DV passthrough (`bHDRPassthroughRaw`) binds the copy shader to the tone-map + correction + all ML steps so an SDR display shows the unconverted HDR/DV signal.

## Tone Curve: The Unified Model

### Design Decision

Rather than three separate shader passes for shadow/gamma/highlight, a **single unified tone curve** operates on the normalized I channel in `ps_ml_gamma.hlsl`. This was a deliberate collapse: the three XGBoost models each predict a scalar that parameterizes one region of the same curve, and applying them sequentially in one pass avoids three round-trips through ICtCp conversion and texture ping-pong.

The highlight pass (`ps_ml_highlight.hlsl`) is a pure passthrough. The `ps_ml_shadow.hlsl` pass was repurposed as the **micro-contrast** pass and moved to run *after* the gamma tone curve (it operates on the post-gamma I_norm — what the viewer sees). Its dual-band DoG infrastructure (σ=1/2/4 blurs) is retained dormant in the same file.

### Three Curve Regions

#### 1. Shadow Toe (I_norm ∈ [0, knee], default knee = 0.25)

**Model**: `xgb_shadow_recovery.plxgb` — 88 features → scalar [0, 0.4]

**Training target** (from DV L2 metadata):
```
lift_delta    = max(0, trim_2081_offset - 2048) / (2269 - 2048)
shadow_crush  = max(0, -shadow_dev) / 0.10
target        = (lift_delta × 0.60 + shadow_crush × 0.40) × 0.40
```

The target blends two DV colorist signals: the L2 offset pedestal lift (how much the colorist raised the black point) and the shadow crush compensation (how much the native tone map compressed shadows). The 0.40 scale keeps the prediction conservative.

**Shader formula**:
```hlsl
t = 1.0 - I_norm / knee
I_norm += MLShadowToe × I_norm × t²
```

This raises the shadow region toward the knee point. The `I_norm` multiplier preserves true black (I=0 is untouched). The `t²` falloff ensures the lift peaks in the lower midtones and vanishes at the knee — no discontinuity at the shadow/midtone boundary.

#### 2. Gamma (midtone region, anchored with fade-in and guard)

**Model**: `xgb_gamma_model.plxgb` — 88 features → scalar [0.5, 1.5]

**Training target**: `gamma = 2048 / trim_2081_power`
- gamma > 1.0: more contrast (DV colorist boosted midtone S-curve, typical for dark drama)
- gamma = 1.0: neutral
- gamma < 1.0: less contrast (rare)

**Shader formula**:
```hlsl
fade = smoothstep(0.08, 0.45, I_norm)      // ramp in from deep shadows
hi   = smoothstep(MLGuard, 0.98, I_norm)   // ramp out near highlights
eff  = fade × (1 - hi)                      // effective strength
I_norm = lerp(I_norm, pow(I_norm, 1/gamma), eff)
```

The `pow(I_norm, 1/gamma)` is the classic gamma lift/lower. The `fade` smoothstep prevents noise amplification in near-black regions. The `hi` smoothstep (governed by `MLGuard`) protects specular highlights from clipping. `MLGuard` is scene-adaptive: `guard = max(0.50, 0.85 - (l1max - 0.6) × 0.75)` — bright scenes push the guard lower to protect a wider highlight range.

**Temporal smoothing**: In Auto mode, gamma predictions are IIR-filtered (α=0.10) to prevent flicker across frames. Scene cuts (detected via L2 distance across 5 histogram features) reset the filter to the raw prediction.

#### 3. Highlight Rolloff (I_norm ∈ [knee, 1.0], default knee = 0.70)

**Model**: `xgb_highlight_rolloff.plxgb` — 88 features → scalar [-0.3, 0.5]

**Training target**: The delta between the DV gold (mastered) curve and the libplacebo spline baseline at the top two knots of the 256-point PQ curve:
```
highlight_delta = mean(gold[knot_6], gold[knot_7]) - mean(spline_km[6], spline_km[7])
```

This is a signed scalar:
- Positive: colorist expanded the shoulder (brighter highlights)
- Negative: colorist compressed the shoulder (darker, more filmic rolloff)

**Shader formula**:
```hlsl
span     = max(1.0 - knee, 0.01)
t        = (I_norm - knee) / span
shoulder = MLHighlightRolloff × t² × (1 - I_norm)
I_norm   = saturate(I_norm + shoulder)
```

The `t²` gives a smooth onset from the knee. The `(1 - I_norm)` factor naturally tapers to zero at peak white, preventing hard clipping.

### Mode Selection

| Mode | Behavior |
|------|----------|
| **Auto** | All three models predict per-scene; gamma is IIR-filtered; shadow gated on tone curve enable |
| **Manual** | User sets Shadow [0, 0.4], Gamma [0.5, 1.5], Highlight [-0.3, 0.4] via s3_shell sliders |
| **Off** | All three default to neutral (shadow=0, gamma=1, rolloff=0) |

## Chroma Vector Tuning (Spectrum Shift v2)

### Design Decision: ICtCp Ct/Cp Space

Chroma boost operates on the Ct and Cp channels of ICtCp rather than Cr/Cb of YCbCr. In ICtCp, equal numeric changes in Ct/Cp correspond to equal perceived color differences (perceptual uniformity). This means:
- Lower scaling factors are needed (1.08–1.15 vs 1.20–1.35 in YCbCr)
- Uniform boost doesn't oversaturate reds relative to blues
- The chroma clamp can be tighter (max 1.30 vs 1.50) without losing punch

### Smooth Monotonic Curve

The chroma boost follows a luminance-dependent curve designed to match the desaturation profile of tone mapping (highlights lose more saturation than shadows):

```
I_norm ≤ knee:  boost = lerp(1.0, neutral, I_norm / knee)
I_norm > knee:  boost = lerp(neutral, fire, ((I_norm - knee) / (1 - knee))^1.5)
```

- **At black** (I_norm = 0): boost = 1.0 (no boost — avoids amplifying noise in dark regions)
- **At knee** (default 0.45): boost = neutral (default 1.08 — subtle midtone saturation)
- **At peak** (I_norm = 1.0): boost = fire (default 1.15 — compensate tone-map desaturation)

The `pow(t, 1.5)` exponent creates a gentle acceleration into the highlights rather than a linear ramp.

### Skin Protection

A pre-trained 2D LUT maps Cr/Cb coordinates (in gamma-2.2 space) to a skin probability P(skin) ∈ [0, 1]. The chroma boost is suppressed on skin regions:

```hlsl
chroma_scalar = lerp(luma_boost, 1.0, skin_weight × MLChromaSkinProtect)
```

Detection happens in gamma-2.2 Cr/Cb (where the LUT was trained), but the boost is applied in ICtCp Ct/Cp — detection and application are in different color spaces by design.

### Saturation Gain: Why It's Not Trained

Investigation of the DV `trim_2081_sat_gain` field across the training dataset revealed:
- **71.5% between-title variance** — sat_gain is primarily a per-title aesthetic choice, not a per-scene corrective signal
- **49.5% neutral** (gain ≈ 1.0), **47% reduce** (gain < 1.0), mean = 0.9321
- Within-title standard deviation ≤ 0.01 for the majority of titles (it's nearly a constant per show)

This means sat_gain reflects colorist taste (e.g., "this show should be desaturated") rather than scene content. A model trained on it would learn title identity rather than scene features — not useful for a universal renderer. The chroma curve uses fixed defaults instead.

### Default Parameters

| Parameter | Value | Range | Rationale |
|-----------|-------|-------|-----------|
| Neutral (Ch-N) | 1.08 | [1.00, 1.30] | Subtle midtone boost, conservative for ICtCp |
| Fire (Ch-F) | 1.15 | [1.00, 1.30] | Compensate highlight desaturation without oversaturating |
| Knee | 0.45 | [0.10, 0.90] | Below midpoint — dark-region noise gets no boost |
| Skin protect | 0.95 | [0.50, 1.00] | Strong skin suppression, small gap for natural warmth |

## Removed Features and Rationale

### Hunt Compensation — Removed

Hunt effect (perceived colorfulness increases with luminance) is already handled by ICtCp's perceptual uniformity. In YCbCr or CIE Lab spaces, Hunt compensation is needed because equal numeric chroma at different luminances looks different. In ICtCp, Ct/Cp are designed to be perceptually uniform across luminance — the effect is baked into the color space transform. The Hunt slider (`m_fMLHunt`) is hardcoded to 0.0 in s3_shell; the cbuffer field is retained for layout stability.

### DoG Shadow Enhancement — Disabled

The dual-band Difference-of-Gaussians (DoG) detail enhancement was architecturally complete but disabled for gradient/banding artifacts. Its σ=1/2/4 bilinear-optimized Gaussian functions remain in `ps_ml_shadow.hlsl` as dormant infrastructure; the pass's `main` now runs the micro-contrast clarity kernel instead (see above). Re-enabling the DoG bands is future work — and the micro-contrast design supersedes them for mid/high texture.

The three user-touch sections that were removed in the 2026-09-07 cycle — original SHADOW (DoG sliders), CONTRAST, FACE DETECT — were also removed from the s3_shell UI. Of these, CONTRAST returned as **MICRO CONTRAST** (new section below).

### Micro-Contrast — Texture Enhancement

**Design**: multiplicative clarity — a fine spatial-frequency local contrast lift, applied to the ICtCp I channel *after* the tone curve. The per-pixel deviation of the gamma-2.2 luma surface from its σ≈1 surround is lifted in log space:

```hlsl
lm   = γ2.2 luma(this pixel)
b    = blur_σ1(lm)                     // bilinear-optimized Gaussian
logr = clamp(log(lm / max(b, 1e-5)), -1, 1)
zone = smoothstep(MCZoneLo, MCZoneLo + 0.15, I_norm)   // shadow gate
I_out = I_norm * exp(MCStrength * zone * logr)
I_out = min(I_out, I_norm * (1 + MCRatioCap * MCStrength))  // excursion bound
```

The multiplicative / ratio form makes halos impossible, and smooth gradients are identity (`lm ≈ b` → ratio ≈ 1) — the artefact class that killed the earlier DoG band does not occur.

**Automatic per-scene gate (no ML model)**: a runtime texture-energy metric — the fraction of pixels in the existing 256×144 luma downsample whose luma deviates from the 4-neighbor mean by >5% PQ — is IIR-filtered (α=0.10, same scene-jump reset as gamma) and multiplies the slider into `MCStrength`. Flat/gradient scenes gate to ~0 strength; textured scenes get the lift. This is why micro-contrast needed no model: no texture data exists in the DV training set, and the metric is ~1 ms to compute directly.

**Parameters** (`MCStrength` now carries slider × texture-gate):

| Param | Default | Range | Role |
|-------|--------|-------|------|
| Strength | 0.15 | [0.00, 0.50] | Clarity exponent ceiling |
| Zone lo | 0.15 | [0.00, 0.60] | Shadow gate floor (I_norm) |
| Ratio cap | 1.40 | [1.00, 2.00] | Bright-side excursion cap |

`MCSkinProtect` is unused — the pass touches only the I channel, so skins are inherently safe.

## Sky Semantic Segmentation (Sky Tone Leg)

### Model and Runtime

**Model**: `yolo26n-sem` (Ultralytics semantic segmentation, **Cityscapes 19-class, sky = class index 10**), exported to `yolo26n-sem.onnx` (opset 12, 640×640).

**Runtime**: ONNX Runtime + **DirectML** EP (GPU inference, no CUDA dependency), mirroring the `CFaceDetect` pattern exactly:
- `onnxruntime.dll` pre-loaded from next to the `.ax` (`/DELAYLOAD` keeps an eager resolver from grabbing a stale System32 copy)
- `Ort::InitApi` under an SEH guard; session with `ORT_ENABLE_ALL`, one intra-op thread, DirectML EP (CPU fallback caught)
- Input `images`: float `[1,3,640,640]` RGB, 0..1, gray-114 letterbox
- Output `output0`: uint8 `[1,640,640]` **argmax class-index map** (Cityscapes class per pixel)

### Preprocessing / Decode Contract

`CSkySeg::DetectSky()` consumes a **640×360 SDR-ized BGRA8 preview** — rendered every ~10 frames by the *same* `ps_ml_face_down` shader used for face detection (PQ→SDR is already the right domain for Cityscapes). Letterbox fit into 640×640 (aspect preserved, gray-114 padding), bilinear to NCHW float, run, then:

- **Crop the letterbox padding** — only `[padX..padX+sW, padY..padY+sH]` is copied into the output map (`default = 640×360`). The pad must NOT reach the shader: its top rows would read as a hard horizontal "not-sky" band.
- Mask values 255 = sky / 0 = not, uploaded as an `R8_UNORM` 640×360 texture (`m_SkyMask`, `UpdateSubresource` with row-pitch = `sW`), sampled as `t2` on shader slots 2.
- `sky_frac` (over valid pixels) and `sky_luma` (mean gamma-2.2 luma of sky) are reported separately for debug/slider use.

Cadence: throttled `m_MLDebugFrame % 10 == 0` (~10 fps inference on the render thread); the mask persists between runs; all per-frame log output gates on the frame-log toggle (`m_bMLDebug`, so a 3-hour movie without frame log grows no log).

### UV Remap (why the mask aligns with the video)

The mask is content-aligned (`[0,1]` = the video content). The gamma/MC shaders sample the window-sized post-scale texture, where the content sits at `uvOff..uvOff+uvSc`. Every ML shader therefore remaps before sampling:

```hlsl
float2 cuv = (input.Tex - float2(MLSkyUVOffX, MLSkyUVOffY))
           / max(float2(MLSkyUVScX, MLSkyUVScY), 0.001f);
// in-content gate: 0 outside [0,1] so letterbox/pillarbox bars never read the
// clamped mask edge (this bled sky into the top bar before the gate existed)
const float inContent = (all(cuv >= 0.0) && all(cuv <= 1.0)) ? 1.0 : 0.0;
float S = sky_mask.Sample(sampSky, saturate(cuv)).r * inContent;
```

The four UV fields (`MLSkyUVOffX/Y`, `MLSkyUVScX/Y`) ride the per-frame cbuffer refresh (`rect`/`inputW` computed in the ML render block), so the mask tracks the content rect on every frame — including pillarbox/letterbox. The cyan debug overlay samples the *same* `cuv` so what you validate visually is exactly what the tone leg targets.

### Tone Effects (all gated by `S` = mask × `inContent`)

All effects scale with `MLSkyStrength` (default 0.6, slider) and vanish at strength 0 — output is bit-identical with the pre-sky path.

1. **Guard relaxation in the gamma leg** — the highlight guard is raised *inside* sky (`hi_s = smoothstep(MLSkyGuard, 0.98, I_norm)` vs the global `hi_g`), so the shoulder protects a wider range in sky; `I_norm = lerp(I_g, I_s, S × MLSkyStrength)` — both legs share the same curve family below the guard band, so there is no seam at the building/sky boundary.
2. **Softened highlight rolloff** — `skyScale = 1.0 − 0.5×S×MLSkyStrength` thins the shoulder crush only inside sky.
3. **Cloud-band lift** — a small extra allocation in the upper-mid luma band where clouds live (below the guard, which the guard-leg alone can't reach):
   ```hlsl
   band = smoothstep(0.40, 0.55, I_norm) * (1 − smoothstep(0.85, 0.95, I_norm));
   I_norm += S × MLSkyStrength × 0.06 × band;
   ```
4. **Sky-upweighted micro-contrast** — in `ps_ml_shadow.hlsl` the clarity lift scales by `(1 + 2×S)` so cloud layering survives tone compression; flat sky is unaffected (`logr ≈ 0`).

### Settings

| Setting | Key | Default | Role |
|---------|-----|---------|------|
| Enable | `zion.skyEnabled` | off | Runs the segmenter + tone effects |
| Strength | `zion.skyStrength` | 0.60 | Global scale for all four effects |
| Mask overlay | `zion.skyDebugVis` | off | Cyan overlay at content UV (validation) |

## Face Detection

### Architecture

Face detection uses **YuNet** (227KB ONNX model) via ONNX Runtime with the DirectML execution provider (GPU inference on all Windows 10+ GPUs — AMD, Intel, NVIDIA without CUDA dependency).

```
Per frame:
  1. GPU downsample source → 480×288 BGRA8 (ps_ml_face_down.hlsl)
  2. CopyResource → staging texture, Map to CPU
  3. BGRA→BGR strip + HWC→NCHW float32 conversion
  4. DetectSync() — ORT Session::Run with DirectML EP
  5. Decode 12 output tensors → face boxes (CPU)
  6. ByteTrack — Kalman filter + Hungarian assignment for persistent IDs
  7. Pack face boxes into MLParams cbuffer (up to 4 faces)
```

### ByteTrack Integration

Raw per-frame detections are fed into a ByteTrack multi-object tracker that provides:
- **Persistent track IDs** across frames (survives brief occlusions)
- **Kalman-predicted positions** when detection drops a frame
- **Scene cut reset** — when L2 distance across 5 histogram features exceeds 0.12, the tracker is re-initialized to prevent stale tracks from a previous scene

Track IDs are passed to the shader via `FaceTrackId[4]` for debug visualization (each track gets a unique hue derived from its ID via the golden ratio).

### Shader Integration

Face boxes are available in the gamma shader cbuffer as `FaceBox[4]` (UV coordinates) with `FaceProtect`, `FaceFeather`, and `FaceExpand` parameters. The current gamma shader does **not** use face boxes for protection (the `skin_blend` variable is hardcoded to 0.0) — face detection is wired for debug visualization only. The infrastructure is ready for face-guided gamma protection when needed.

Face box UV coordinates are remapped from the convert texture [0,1] space to the post-scale shader UV space when the video is letterboxed/pillarboxed in a larger surface.

## Feature Extraction

### Pipeline

A 256×144 R16_UNORM luma texture is rendered via `ps_ml_luma.hlsl` (2×2 box-averaged downsample of the BT.709 luma channel), copied to a CPU-readable staging texture, and processed into 88 features:

- **Histogram features**: percentiles (p1, p5, p10, p25, p50, p75, p90, p95, p99), mean, std, skewness, kurtosis
- **MaxSCL features**: scene maximum content light (from HDR metadata or measured)
- **Saturation features**: spatial statistics of per-pixel saturation in 5×5 blocks
- **Spline knot features**: positions of the 8-knot BT.2390 spline curve relative to MaxSCL

These mirror the `dv_coef_model.py` canonical feature set used during training.

### Model Format

Models are exported as `.plxgb` binary files (magic `PLXG`, version 1) — a compact native format that `CMLModel` loads and evaluates without the full XGBoost library. Each model is loaded once at pipeline init and evaluated per-frame (~0.1ms CPU).

## s3_shell UI Layout

### TONE CURVE Section

| Control | Type | Key | Range | Default |
|---------|------|-----|-------|---------|
| Mode | Cycle | zion.toneMode | Auto / Manual / Off | Auto |
| Shadow | Slider | zion.shadowToe | 0.00 – 0.40 | 0.00 |
| Gamma | Slider | zion.gamma | 0.50 – 1.50 | 1.00 |
| Highlight | Slider | zion.highlight | -0.30 – 0.40 | 0.00 |

### CHROMA Section

| Control | Type | Key | Range | Default |
|---------|------|-----|-------|---------|
| Mode | Cycle | zion.chromaMode | Default / Manual / Off | Default |
| Ch-N | Slider | zion.chN | 1.00 – 1.30 | 1.08 |
| Ch-F | Slider | zion.chF | 1.00 – 1.30 | 1.15 |
| Knee | Slider | zion.knee | 0.10 – 0.90 | 0.45 |
| Skin | Slider | zion.skin | 0.50 – 1.00 | 0.95 |

### MICRO CONTRAST Section

| Control | Type | Key | Range | Default |
|---------|------|-----|-------|---------|
| Enable | Toggle | zion.contrast | — | on |
| Strength | Slider | zion.contrastStrength | 0.00 – 0.50 | 0.15 |
| Zone lo | Slider | zion.contrastZoneLo | 0.00 – 0.60 | 0.15 |
| Ratio cap | Slider | zion.contrastRatioCap | 1.00 – 2.00 | 1.40 |

### DEBUG Section

| Control | Type | Key | Notes |
|---------|------|-----|-------|
| HDR passthrough | Toggle | hdr.hdrPass | Raw HDR/DV on SDR — copies tone-map + correction + all ML steps (no processing at all). Mutually exclusive with Tone curve passthrough. |
| Tone curve passthrough | Toggle | zion.debugCopyShaders | Binds copy shader to the ML steps only (ring step count intact). Mutually exclusive with HDR passthrough. |
| Frame log | Toggle | zion.debugFrameLog | Gates every per-frame log line (EXTRACT, MLPASS, SKY_REFRESH, SKYUV, FACE_SYNC…) — log stays flat when off |
| Face boxes | Toggle | zion.faceDebugVis | Track-colored ellipses (window UV) |
| MC heat | Toggle | zion.contrastDebugVis | Green zone gate overlay (independent of strength) |

Both passthrough toggles push **both** configs when either flips, so disabling the last passthrough retriggers the current Zion/MlSettings and ML re-applies cleanly.

### SKY SEG Section

| Control | Type | Key |
|---------|------|-----|
| Enable | Toggle | zion.skyEnabled |
| Strength | Slider | zion.skyStrength |
| Mask overlay | Toggle | zion.skyDebugVis |

### HDR TONE MAPPING Section

| Control | Type | Key | Range/Default |
|---------|------|-----|---------------|
| Curve | Cycle | hdr.toneMapCurve | ACES / Reinhard / Hable / Mobius / BT-2390, default BT-2390 |
| SDR nits | Slider | hdr.sdrNits | default 50 (setpoint the SDR display is tuned to) |

### BLACK BARS Section

| Control | Type | Key |
|---------|------|-----|
| Repaint bars | Toggle | zion.barsRepaint |
| Highlight bars | Toggle | zion.barsHighlight |

## Constant Buffer Layout (MLParams_t)

```
Offset  Field               Size    Notes
────────────────────────────────────────────────────────
0       MLGamma             4       [0.5, 1.5]
4       MLShadowToe         4       [0.0, 0.4]
8       MLShadowKnee        4       0.25 fixed
12      MLShadowStrength    4       0.0 (DoG disabled)
16      MLHighlightRolloff  4       [-0.3, 0.5]
20      MLHighlightKnee     4       Adaptive on gamma
24      MLGuard             4       Adaptive on l1max
28      MLHunt              4       0.0 (disabled)
32      MLL1Max             4       Scene peak in PQ
36      MLHiDesat           4       Highlight desaturation
40      IsPQ                4       1.0 = PQ, 0.0 = SDR
44      Padding             4       —
48      MLChromaNeutral     4       1.08
52      MLChromaFire        4       1.15
56      MLChromaKnee        4       0.45
60      MLChromaSkinProtect 4       0.95
64      MLSkinLUTCR0        4       Skin LUT grid bounds
68      MLSkinLUTCR1        4
72      MLSkinLUTCB0        4
76      MLSkinLUTCB1        4
80      MLChromaGate        4       1.0 = chroma active
84      MLHuntGate          4       0.0 (disabled)
88      _reserved[6]        24      —
112     uBarTop             4       Bar bounds
116     uBarBottom          4
120     uBarV0              4
124     uBarSpan            4
128     uBarFeather         4
132     uBarBlack           4
136     uBarHighlight       4
140     _padBar             4       Alignment
144     MCStrength          4       slider × automatic texture gate [0, 0.5]
148     MCZoneLo            4       shadow gate floor 0.15
152     MCSkinProtect       4       1.0 (unused — I-channel pass)
156     MCRatioCap          4       excursion cap 1.40
160     FaceProtect         4       0.8
164     FaceFeather         4       0.12
168     FaceExpand          4       2.0
172     FaceDebugVis        4       Toggle
176     NumFaces            4 (int) 0–4
180     _padFace[3]         12      Alignment
192     FaceBox[16]         64      4 × (x, y, w, h) UV
256     FaceTrackId[4]      16      ByteTrack persistent IDs
272     MLSkyStrength       4       0..1 sky-leg blend (0 = bit-identical pre-sky path)
276     MLSkyGuard          4       Raised guard used inside sky
280     MLSkyLuma           4       Sky-region mean luma (informational)
284     MLSkyUVOffX         4       Mask-UV remap — content sits at
288     MLSkyUVOffY         4         uvOff..(uvOff+uvSc) in window UV
292     MLSkyUVScX          4
296     MLSkyUVScY          4
300     _padSky             4       Keep sizeof a multiple of 16 (D3D11 rule)
────────────────────────────────────────────────────────
Total: 304 bytes (= 16 × 19)
```

`pPSSetConstantBuffers` uploads `m_lastMLParams` every frame from the ML render block (also refreshing the four sky-UV fields from `rect`/`inputW`), so the remap tracks the content rect on every frame even when the mask only refreshes at ~10 fps.

## File Map

### Shaders (`Shaders/d3d11/`)

| File | Purpose | State |
|------|---------|-------|
| `ps_ml_luma.hlsl` | 256×144 luma downsample for feature extraction | Active |
| `ps_ml_face_down.hlsl` | 480×288 + 640×360 color downsample for face and sky preprocessing | Active |
| `ps_ml_gamma.hlsl` | Unified tone curve (shadow + gamma + highlight) + sky tone leg + face/sky debug vis | Active |
| `ps_ml_shadow.hlsl` | Micro-contrast clarity (post-gamma; DoG infra dormant) + sky-upweighted MC | Active |
| `ps_ml_highlight.hlsl` | Highlight rolloff (merged into gamma) | Passthrough |
| `ps_ml_chroma.hlsl` | ICtCp Ct/Cp chroma boost with skin LUT protection | Active |
| `ps_ml_bars.hlsl` | Black bar repaint for projector haze | Active |

### C++ Source (`Source/`)

| File | Role |
|------|------|
| `DX11VideoProcessor.h` | `MLParams_t` cbuffer struct, ML state variables, XGBoost model instances, sky seg members |
| `DX11VideoProcessor.cpp` | `SetMLParams()` (model inference + parameter packing), `ExtractMLFeatures()`, render pipeline hookup, sky mask texture + UV remap |
| `face_detect.h` / `face_detect.cpp` | YuNet ONNX wrapper — init, preprocess, inference, NMS, sync detection |
| `sky_seg.h` / `sky_seg.cpp` | yolo26n-sem ONNX wrapper — letterbox preprocess, DirectML run, pad-crop decode, sky_frac/luma |
| `onnxrt/yolo26n-sem.onnx` | Sky segmentation model (Cityscapes 19-class, sky = class 10) |

### Training Scripts (`libplacebo/ml/`)

| File | Model | Target |
|------|-------|--------|
| `train_xgb_l2_power.py` | Gamma | `2048 / trim_2081_power` |
| `train_xgb_shadow_model.py` | Shadow toe | Blended L2 offset + shadow crush |
| `train_xgb_highlight_model.py` | Highlight rolloff | Gold vs spline knot deviation |
| `train_xgb_sat_gain.py` | Saturation gain | `trim_2081_sat_gain / 2048` (NOT deployed) |

### UI (`mpc-hc/tools/engine_s3/s3_shell.cpp`)

Controls panel with TONE CURVE, CHROMA, MICRO CONTRAST, DEBUG, and BLACK BARS sections. Removed: SHADOW (DoG) and FACE DETECT sections. Shadow toe slider lives in TONE CURVE alongside Gamma and Highlight.

## Current State (2026-09-09)

### Working

- Unified tone curve: three XGBoost models → single shader pass on ICtCp I channel
- Auto mode: per-scene gamma, shadow toe, and highlight rolloff prediction with IIR temporal smoothing
- Manual mode: all three tone curve parameters user-adjustable via s3_shell sliders
- ICtCp chroma boost with smooth monotonic curve and skin LUT protection
- Micro-contrast texture enhancement with an automatic per-scene texture gate
  (IIR-filtered high-frequency luma edge fraction × user strength)
- Face detection via YuNet + ByteTrack with debug visualization
- **Sky semantic segmentation** (yolo26n-sem, ONNX Runtime + DirectML): sky tone leg
  (guard relaxation + softened rolloff + cloud-band lift) and sky-upweighted micro-contrast,
  with a mask-overlay validation toggle
- **Sky mask UV remap** — mask sampled at content UV `(input.Tex − uvOff)/uvSc` with an
  in-content gate, so the tone leg + overlay track the video rect under pillarbox/letterbox
- Black bar repaint for projector haze
- Scene cut detection resets the gamma IIR filter and ByteTrack tracker
- HDR/DV passthrough (raw, no processing) + Tone curve passthrough (ML steps copied),
  mutually exclusive, both retrigger the current ML config on toggle-off
- Tone curve selector (ACES / Reinhard / Hable / Mobius / BT-2390) + SDR nits default 50

### Disabled / Passthrough

- DoG shadow detail enhancement (superseded by micro-contrast; σ=1/2/4 blurs dormant)
- Hunt compensation (redundant in ICtCp)
- Face-guided gamma protection (infrastructure ready, not activated)
- Saturation gain model (per-title aesthetic, not scene-corrective)
- Sky tone effects at strength 0 (bit-identical with the pre-sky path)

### Debug Logging Contract

All per-frame log lines (EXTRACT, MLPASS, FACE_SYNC, SKY_REFRESH, SKYUV, MC_TRACE…) gate on the
frame-log toggle (`zion.debugFrameLog` → `m_bMLDebug`), and `CSkySeg`/`CFaceDetect` receive the
same `SetLogEnabled`. With frame log off, the log file is flat during any-length playback; with it
on, the ML render block writes `MLPASS … SKYUV=(uvOffX,uvOffY,uvScX,uvScY) frac=.. luma=..` every
60 frames and `SKY_REFRESH` on each mask refresh, so UV-mapping issues are diagnosable offline.

### Build

- MPCVR: `MpcVideoRenderer.sln` → Release x64 via MSBuild (`_rebuild_mpcvr.bat`; batches shader + CSO + resource compile)
- s3_shell: `build_s3_shell.bat` via cl.exe (mpc-hc repo, branch `zion-s3-shell`)
- Distribution: `_bin\MpcVideoRenderer-0.10.8.2603\` with Install/Uninstall scripts
- Dependencies: ONNX Runtime + DirectML DLLs, YuNet + `yolo26n-sem.onnx` models, XGBoost `.plxgb` model files
