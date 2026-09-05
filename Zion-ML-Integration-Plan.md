# MPC Video Renderer — Zion ML Integration Plan

**Status:** Draft for review · **Target:** `master`, midtone/shadow/highlight XGBoost grade
**Source of truth:** `C:\Code\libplacebo\src\ml_render.c`, `ml_model.c`, `ml_features.c` and training manifests in `C:\Code\libplacebo\ml\models\*.manifest.json`
**Scope decision:** DX11 path only (mirrors `bVPRTXVideoHDR` / Super Resolution treatment). DX9 path gets a no-op.
**Out of scope (for now):** native-SDR content enhancement — the SDR→P5 virtual-master bridge, `sdr_nits_to_pq` SDR decoding, and SDR feature-floor logic are **deferred** (added complexity, low near-term payoff). The ML grade runs **only on HDR source content** — both HDR passthrough and HDR→SDR conversion output remain in scope, which is where the models add the most value.

---

## 1. What we are porting (the contract)

Three additive-tree XGBoost regressors, identical 88-entry input vector, all inference on CPU (<0.1 ms/frame).

| Model | Binary | Output | Clip | Application (libplacebo hook) |
|---|---|---|---|---|
| Midtone "Oracle" | `xgb_gamma_model.plxgb` | `gamma` | [0.5, 1.5] | **L2 gamma** — Y power curve, fade-in `smoothstep(0.08,0.45)`, highlight guard, hunt/saturation compensation |
| Shadow toe lift | `xgb_shadow_recovery.plxgb` | `shadow_toe` | [0, 0.4] | **Shadow bilateral** — `y += toe·(y/knee)·(1−y/knee)²`, knee 0.25, film-grain gate |
| Highlight rolloff | `xgb_highlight_rolloff.plxgb` | `highlight_rolloff` | [−0.3, 0.4] | **Highlight bilateral** — `y += rolloff·t²·(1−y)` above knee, grain gate |

### 88-feature vector (built by `build_model_features`, [ml_render.c:919-982](C:/Code/libplacebo/src/ml_render.c#L919-L982))

| Slots | Content | Source |
|---|---|---|
| [0..8] | `maxscl` (PQ peak), `average` (PQ APL), `fraction_bright` (>0.5 PQ), percentiles p25/p50/p75/p90/p95/p99 | [ml_features.c](C:/Code/libplacebo/src/ml_features.c) — histogram/percentile reduction of a downscaled luma texture |
| [9..26] | 3×3 grid luma means + maxes | zone reduction (SAT) |
| [27..76] | 5×5 grid luma means + maxes | zone reduction (SAT) |
| [77..84] | 8 knots of libplacebo **spline** tone-map LUT at `{0,36,73,109,146,182,219,255}` of 256 | `pl_tone_map_generate`, `output_max=0.5444` (≈204 nits PQ) |
| [85] [86] | `top_bar_norm`, `bottom_bar_norm` (letterbox) | renderer |
| [87] | `compression_strength = 0.5444 / max(maxscl, floor)` | HDR soft cap at 0.90 PQ; SDR floor (0.62) not needed — native SDR is out of scope |

All luma stats are **PQ-domain, 0..1**.

### Derived values after prediction ([ml_render.c:1001-1150](C:/Code/libplacebo/src/ml_render.c#L1001-L1150))

```
l2_power      = 2048.0f / gamma
l2_saturation = 2048.0f
guard         = 0.85  |  if l1max > 0.6: max(0.50, 0.85 − (l1max−0.6)·0.75)
highlight_knee= clamp(0.90 − (1/safe_gamma)·0.20, 0.65, 0.85)
shadow_knee   = 0.25
```
Plus IIR alpha 0.10 smoothing and scene-cut reset on `|Δl1max| > 0.15`. (No SDR attenuation logic — libplacebo's `sdr_attenuation` only exists for native-SDR source, which is out of scope.)

---

## 2. Design decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | **DX11 only** | DX9 has no HDR path of this kind; feature readback worst-case there. Existing precedent (`RTX Video HDR` is x64-only). |
| D2 | **Precompiled `.cso` shaders** (3 grade passes + 1 downsample) | Matches `ps_hdr10_tonemap.hlsl` pipeline exactly (fxc → RCDATA → `CreatePShaderFromResource`, [DX11VideoProcessor.cpp:798](Source/DX11VideoProcessor.cpp#L798)). No JIT. |
| D3 | **CPU-side feature reduction on a tiny readback** | Port `extract_features_cpu` ([ml_features.c:448-693](C:/Code/libplacebo/src/ml_features.c#L448-L693)) 1-to-1. **256×144** R16 staging (libplacebo's default `PL_ML_FEATURE_DEFAULTS`, [ml_features.h:82-83](C:/Code/libplacebo/src/include/libplacebo/ml_features.h#L82)) = 36,864 px ≈ 72 KB map/frame. No GPU histogram/SAT passes to maintain. madVR-style cost, sub-ms. |
| D4 | **Port `pl_tone_map_spline` for knots [77..84]** | Model training saw these exact knots. Port is ~50 lines (`tone_mapping.c:552-599` + `st2094_pick_knee:228`) + `pl_tone_map_params_infer` (`:78-111`). Do NOT substitute MPCVR's BT.2390 curve — distribution shift. |
| D5 | **Grade = 3 post-scale passes** mirroring libplacebo order shadow→highlight→gamma | In-mechanism: each `TextureCopyRect` step already handles a neighborhood-sampling PS (the bilateral grain gates read neighbor texels). Faithful hook order. |
| D6 | **Models shipped as external files next to the .ax** | 3×~1.4 MB; embedding as RCDATA bloats the DLL. Resolve via module path (`GetModuleFileName`), same directory as filter. Fall back silently to manual mode if missing. |
| D7 | **Phase-gated feature extraction** | Manual mode has **no** feature dependency — delivers working grade before any analytics code exists (see §9). |

---

## 3. Model runtime port

**New file:** `Source/ml_model.cpp` (+ `Source/ml_model.h`) — verbatim port of [ml_model.c](C:/Code/libplacebo/src/ml_model.c) (168 lines). It is dependency-free C; compiles unchanged as C++ under MSVC (`calloc`/`fread` only). Keep structure:

- `struct MLModel { uint32_t feature_count, tree_count; float base_score, learning_rate; ModelTree* trees; }`
- `MLModel* MLModelLoad(const wchar_t* path)` (expose both the binary layout *and* the version/magic check — model mismatch must be a hard fail with a log line, not silent)
- `bool MLModelPredict(const MLModel*, const float* features, int n, float* value)` — the additive leaf walk, [ml_model.c:95-121](C:/Code/libplacebo/src/ml_model.c#L95-L121)
- Ownership: one shared model set owned by the filter (`CMpcVideoRenderer`), lazily loaded in `SetDevice`/`InitDevice` — **never in `Render`/`ProcessSample`** (the known vo_gpu_next crash pattern).

Seed byte layout for the test harness (§9): magic `0x47584c50`, version 1, then `feature_count, tree_count, reserved, base_score, learning_rate` (each 4B), then per-tree `node_count` + nodes + weights. All floats ≤ 1.5 MB per file.

**Build:** add to `MpcVideoRenderer.vcxproj` `<ClCompile Include="ml_model.cpp"/>` + `<ClInclude Include="ml_model.h"/>`. `UpdatePostScale...` untouched.

---

## 4. Feature extraction (Phase 1+)

### 4.1 GPU downsample pass — `ps_ml_luma_downsample.hlsl`

- Signature: fullscreen `ps_4_0`, samples `tex : t0` with bilinear sampler, box-average downsample to a **256×144** target (libplacebo's standard; keeps 16:9 at common content resolutions), emits **luma × (PQ flag)** as `R16_UNORM`.
- **Single mode (HDR source only):** `y = dot(rgb, (0.2126,0.7152,0.0722))` of `m_TexConvertOutput` (already PQ, no conversion branch needed — no SDR path).
- **Geometry note:** render the content rect into a fixed 256×144 region with black bars outside (letterbox/pillarbox preserved). This matters for non-16:9 HDR content (2.35:1 etc.): the [85]/[86] bar-norm features encode exactly that bar fraction, and libplacebo fills the same way, so the zone features [9..76] stay on-policy.

### 4.2 Readback plumbing (new members on `CDX11VideoProcessor`)

```
ID3D11Texture2D* m_MLDownsampleTex;   // 256×144 R16_UNORM, USAGE_DEFAULT, RTV|SRV
ID3D11Texture2D* m_MLStagingTex;      // same dims, USAGE_STAGING + CPU_ACCESS_READ
float  m_MLLuma[256*144];             // CPU mirror
uint8_t m_MLFeatureRefresh;           // once/frame guard
```
- Create/recreate in device init (`Init`/`SetDevice`), release in `ReleaseDevice` alongside the other per-device objects ([DX11VideoProcessor.cpp:665](Source/DX11VideoProcessor.cpp#L665) area).
- **Per-frame flow** (in `ProcessSample`, after `CopySample`, before the first `Render`):
  1. `RenderStep` the downsample PS → `m_MLDownsampleTex` (reuse `TextureCopyRect` machinery or one explicit quad draw),
  2. `CopyResource(m_MLStagingTex, m_MLDownsampleTex)`,
  3. `Map(READ)` → `memcpy` → `Unmap`,
  4. CPU reduction (§4.3) → feature vector → kernel-scene-cut/IIR update → 3 predictions → write cbuffer.
- **Interlaced double-rate**: run once per source frame (guard `m_MLFeatureRefresh` set in `ProcessSample`).

### 4.3 CPU reduction — `ml_features.cpp`

Port `extract_features_cpu` (68 lines of pure math, no GPU):
- max/average/`>0.5` fraction + percentile from a **65536-bin uint16 histogram** (matches `numpy.percentile` semantics via linear interpolation).
- 3×3 and 5×5 zone means + maxes exactly per [ml_features.c:624-684](C:/Code/libplacebo/src/ml_features.c#L624-L684) layout (`zones[c][r]`, not row-major — the manifest feature names confirm column-major indexing: `r0_c0..r0_cN`).
- knots [77..84] via §5; [85]/[86] letterbox norm; [87] compression with HDR soft cap (0.90 PQ). No SDR floor — out of scope.

**Validation gate (do not skip):** log features[0..8] for one HDR file, compare to a Python reference calling `xgb.Booster().predict()` on `extract_features_cpu` logic — must match to ≤1e-4 before wiring AUTO mode (§9.3 acceptance).

---

## 5. Spline knot port (Phase 2)

**New file:** `Source/tone_spline.cpp` — port from libplacebo `tone_mapping.c`:
- `pl_tone_map_params_infer` ([tone_mapping.c:78-111](C:/Code/libplacebo/src/tone_mapping.c#L78-L111)) — clamp logic + `fix_constants` defaults (copy the `PL_TONE_MAP_CONSTANTS` values verbatim).
- `spline()` + `st2094_pick_knee` ([tone_mapping.c:228](C:/Code/libplacebo/src/tone_mapping.c#L228), [:552-599](C:/Code/libplacebo/src/tone_mapping.c#L552-L599)) and the `pl_tone_map_generate` loop ([tone_mapping.c:147-165](C:/Code/libplacebo/src/tone_mapping.c#L147-L165)).
- Call exactly as `build_model_features` does ([ml_render.c:931-967](C:/Code/libplacebo/src/ml_render.c#L931-L967)): `input_max = max(l1_max_pq, features[0])`, `input_avg = l1_avg`, `output_max = 0.5444`, PQ scaling, 8 knots.

---

## 6. Grade passes and pipeline insertion

### 6.1 Three shaders (`Shaders/d3d11/`)

| File | Body source (ml_render.c) | Constants |
|---|---|---|
| `ps_ml_shadow.hlsl` | `shadow_body` L555-594 (toe-lift + dehaze) | `shadow_toe, shadow_knee=0.25, shadow_dehaze, luma_w` |
| `ps_ml_highlight.hlsl` | `highlight_body` L768-785 | `hi_rolloff, hi_knee` |
| `ps_ml_gamma.hlsl` | `l2_hook` body_a L66-116 (gamma + hunt + desat + hue) | `l2_power, l2_saturation, l2_guard, l2_hunt, l2_l1max, l2_hi_desat` |

- **Domain wrap**: the grade targets **gamma-2.2 encoded** signal. MPCVR stage signal differs by output mode:
  - **HDR passthrough** (`bHdrPassthrough`): stage after `m_pPSHDR10ToneMapping` is PQ-encoded → each PS begins `ST2084ToLinear → LinearToGamma22` and ends `Gamma22ToLinear → LinearToST2084` (`shaders/convert/st2084.hlsl` supplies PQ math; add a small `gamma22` pair).
  - **HDR→SDR conversion** (`bConvertToSdr`, no passthrough, HDR source): convert output is BT.709 gamma ≈γ2.2 → apply the grade **directly** (verify actual transfer at grade point during implementation via the stats string `m_strStatsInputFmt`). Native-SDR content never reaches the grade — gate on `SourceIsHDR()`.
- Grain gate: replicate the 5-neighbor log-variance gate from `shadow_body` (single extra texture sample set per pass — cheap). No alpha channel needed.

### 6.2 cbuffer

```
cbuffer MLParams : register(b0) {
  float MLGamma;        // = 2048/l2_power
  float MLShadowToe;
  float MLShadowKnee;
  float MLShadowDehaze;
  float MLHighlightRolloff;
  float MLHighlightKnee;
  float MLGuard;
  float MLHunt;
  float MLL1Max;        // PQ
  float MLHiDesat;
  float MLPadding[2];
};
```
`struct MLParams_t` in `DX11VideoProcessor.h` (mirror `HDRParamsConstantBuffer_t`), one shared buffer updated with the `SetDolbyVisionDynamicParams` memcmp-cache pattern ([DX11VideoProcessor.cpp:955-982](Source/DX11VideoProcessor.cpp#L955-L982)).

### 6.3 Pipeline — `CDX11VideoProcessor::Process` ([DX11VideoProcessor.cpp:3295](Source/DX11VideoProcessor.cpp#L3295))

Insert 3 steps between the HDR-TM block and the user post-scale block:

```cpp
// ~L3377, after m_pPSHDR10ToneMapping block
if (m_bMLActive) {
    StepSetting();
    hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSMLShadow,    m_pMLConstants, 0, false);
    StepSetting();
    hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSMLHighlight, m_pMLConstants, 0, false);
    StepSetting();
    hr = TextureCopyRect(*pInputTexture, pRT, rect, rect, m_pPSMLGamma,     m_pMLConstants, 0, false);
}
```
- `GetPostScaleSteps()` (:780-796): add 3 when `m_bMLActive` — `UpdatePostScaleTexures` sizes `m_TexsPostScale` from this automatically (:2904-2922).
- **HW-VP bypass**: extend the `bNeedShaderTransform` condition (:3308-3311) with `|| m_bMLActive` so the pure `D3D11VPPass→present` fast path (NVIDIA/AMD/Intel) is not taken while ML is on — same tradeoff HDR local TM already makes.
- Shader create + release: `UpdatePostScaleStepShaders()` style helper called from `Configure`/`SetDevice`, with `m_pPSML*.Release()` added to `ReleaseDevice` (:708-716 area).

### 6.4 Domain anchor notes

- `m_TexConvertOutput` is created at source res in `m_InternalTexFmt` ([DX11VideoProcessor.cpp:2891-2901](Source/DX11VideoProcessor.cpp#L2891-L2901)) — **this is the feature-extraction anchor** (reads PQ RGB just after `ConvertColorPass`/`D3D11VPPass`), independent of upscaling.
- `m_TexsPostScale` works at window res in `m_InternalTexFmt` — the grade output width = window, correct for present.

---

## 7. Per-frame evaluation circuit

New member fn `void EvaluateMLFrame()` (called from `Render()`, next to `SetHDR10ShaderParams` call at :2726):

```
if (!m_bMLActive || !m_MLModelsLoaded) return;
if (!SourceIsHDR()) return;                                 // grade never runs on native SDR
if (AUTO and features stale) { MLFeatureExtract(); }        // via §4 readback
MLModelPredict(gamma?, shadow?, highlight? models)          // per-mode
IIR smooth (alpha 0.10) on auto outputs; scene-cut reset on |Δl1max|>0.15
fill m_lastMLParams cbuffer, UpdateSubresource when changed
```
- **Manual mode** (`m_iMLMode==MANUAL`): scalars straight from settings, no model, no features, IIR bypassed — the Phase 0 deliverable.
- Auto predictions clipped to model clips (§1) before smoothing.
- Rotate which models run: `m_MLMode[3]` (per-model mode, matching libplacebo `gamma_mode/shadow_mode/highlight_mode`) — one global mode and three per-model manual values is the simplest MPCVR-shaped surface (see §8).

---

## 8. Settings surface

`Settings_t` ([IVideoRenderer.h:104-186](Source/IVideoRenderer.h#L104-L186)) — new fields, all defaulted in `SetDefault()`:

```cpp
bool bMLEnabled = false;          // master switch (registry "MLEnabled")
int  iMLGammaMode;                // 0 off / 1 auto / 2 manual
float fMLGamma;                   // manual gamma  [0.5, 1.5]
int  iMLShadowMode;  float fMLShadow;     // [0.0, 0.4]
int  iMLHighlightMode; float fMLHighlight; // [-0.3, 0.4]
// no fMLAttenuation — native-SDR enhancement deferred (out of scope)
```

- **PropPage** (`Source/PropPage.cpp` + `MpcVideoRenderer.rc`): new "Zion ML" groupbox appended below the HDR group (dialog is 352×264; extend to ~300 height; wire via `CPropPage` handlers mirroring the HDR combos :90-111). Minimal Phase-0 rows: master checkbox, mode combo, three spinners.
- **Registry**: settings serialize in `CMpcVideoRenderer::SetSettings/GetSettings` + registry load/store in `dllmain`/`VideoRenderer.cpp` (the existing `Settings_t` path, [VideoRenderer.cpp:162-275](Source/VideoRenderer.cpp#L162-L275) region).
- **MPC-BE interop**: `IExFilterConfig` keys `mlEnabled`, `mlGammaMode/Value`, `mlShadowMode/Value`, `mlHighlightMode/Value` ([Include/FilterInterfaces.h:75-79](Include/FilterInterfaces.h#L75) pattern) — names aligned with the mpv options so the Unplugged front-end can drive both renderers. (No `mlAttenuation` until SDR support is revisited.)
- Stats overlay: extend `UpdateStatsStatic` with a `ML:` line (gamma/shadow/highlight + `model_used/fallback`) in `m_strStatsHDR`.

---

## 9. Phased rollout

| Phase | Scope | Feature deps | Exit criterion |
|---|---|---|---|
| **0** | Model runtime + 3 grade passes + manual mode + settings + pipeline wiring | none | Manual sliders visibly move gamma/toes/shoulder on an HDR sample, dither intact, no crash on video type changes |
| **1** | Feature extraction (§4) + AUTO **gamma** + IIR + scene-cut + stats | readback + reduction | Features[n] match Python reference ≤1e-4; AUTO gamma reacts to scene luma; no frame-rate hit >1 ms |
| **2** | AUTO shadow + highlight + spline knots (§5) + DoVi ordering check | spline port | Predictions match Python on 3 test files; DoVi sample passes through without double-trim |
| **3** *(optional)* | DX9, HW-VP direct analytics | — | — |

> **Deferred (not planned this cycle):** native-SDR enhancement (SDR→P5 virtual-master bridge, attenuation). Revisit after 1–2 releases of the HDR-only path — the shader fold-in points are left in place (`SourceIsHDR()` gate), so re-enabling later is a localized change.

---

## 10. Build & packaging

1. **`Shaders/compile_shaders.cmd`** — add 4 D3D11 lines (`ps_ml_gamma/highlight/shadow/downsample.cso`, `ps_4_0`, `/Fo "_bin\shaders\..."`).
2. **`Source/resource.h`** — new `IDF_PS_11_ML_*` IDs (free range ~890-894; confirm no collision with existing 801..882).
3. **`Source/res/MpcVideoRenderer.rc2`** — `IDF_PS_11_ML_GAMMA FILE "..\_bin\shaders\ps_ml_gamma.cso"` (4 entries) + one `FILE` entry per `.plxgb` **only if embedding** (D6 says external — skip).
4. **`MpcVideoRenderer.vcxproj`** — add `ml_model.cpp`, `ml_features.cpp`, `tone_spline.cpp` (+ headers to filters file).
5. **`build_mpcvr.cmd`** — add the three `.plxgb` to the package zip; copy them next to the .ax after build.
6. **`history.txt`** — 0.10.9 entry when landing.

---

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Feature-vs-training drift (MPCVR ≠ libplacebo pipeline, different tone-map context) | Knot features [77..84] reproduce libplacebo spline exactly (§5); luma stats are renderer-independent. Accept residual drift: models were trained on `extract_features_cpu`-style stats which we port verbatim |
| Readback stalls the render thread | 256×144 map once per frame (~72 KB) is sub-ms and off the present critical path (mirrors madVR); keep `m_MLFeatureRefresh` guard |
| Model file missing/corrupt | Fail soft → manual mode only, stats line reports `fallback` |
| ML + HW VP interplay on AMD/Intel | `bNeedShaderTransform` already precedent from HDR local TM; document the perf note in the options tooltip |
| Double trim with DoVi L2 | Grade runs after `m_pPSHDR10ToneMapping` (which applies DoVi trims), mirroring libplacebo L2-hook placement |
| Interlaced/double-rate | Evaluate once per source frame (guard), constant buffer applies to both fields |
| Highlight rolloff clipping on HDR→SDR conversion output | Grade runs only for HDR source, so no attenuation concern; outputs clamped to [0,1] + highlight guard keep the HDR→SDR conversion in range, and FinalPass dither still follows the grade |

---

## Appendix — exact touch points

| File | Function / line | Change |
|---|---|---|
| `Source/ml_model.cpp/.h` | (new) | port of `ml_model.c` |
| `Source/ml_features.cpp/.h` | (new) | CPU reduction port |
| `Source/tone_spline.cpp/.h` | (new) | spline knot port |
| `Source/DX11VideoProcessor.h/.cpp` | `Init`/`SetDevice`/`ReleaseDevice` (:665,:708) | model load/unload, ML textures, shaders |
| " | `Process()` (:3295-3434) | 3 grade steps + downsample; `bNeedShaderTransform` (:3308) |
| " | `GetPostScaleSteps()` (:780) | +3 when active |
| " | `EvaluateMLFrame()` (new, call site :2726) | prediction + IIR + cbuffer |
| " | `Configure()` (:405-410) | read new `Settings_t` fields |
| " | `SetHDR10ShaderParams` region (:908) | model/feature plumbing pattern reference |
| `Source/VideoRenderer.cpp/.h` | ctor settings load (:162-275) | registry keys |
| `Source/PropPage.cpp` + `MpcVideoRenderer.rc` | dialog | Zion ML group |
| `Source/IVideoRenderer.h` | `Settings_t` (:104) | new fields |
| `Shaders/d3d11/ps_ml_*.hlsl` | (new, ×4) | grade + downsample |
| `Shaders/compile_shaders.cmd` | D3D11 section (:104-109) | 4 fxc lines |
| `Source/resource.h` / `res/MpcVideoRenderer.rc2` | IDF range 801..882 | 4 new IDs + FILE entries |
| `MpcVideoRenderer.vcxproj` | item groups | new sources |
| `build_mpcvr.cmd` | package step (:120-131) | +3 `.plxgb` |
| `history.txt` | — | changelog |

**Estimated size:** ~1,100 new lines C++/HLSL across 8 files, zero new external dependencies.