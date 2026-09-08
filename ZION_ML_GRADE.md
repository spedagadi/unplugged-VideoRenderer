# Zion ML Grade — Technical Reference

## Overview

Zion ML Grade is a real-time video enhancement pipeline for MPC Video Renderer (MPCVR) that applies scene-adaptive tone mapping and chroma tuning using machine learning models trained on Dolby Vision Profile 5 metadata. All luminance operations are performed in the **ICtCp perceptual color space** (BT.2100 / SMPTE ST 2084 PQ transfer), which preserves chrominance (Ct/Cp) when the intensity (I) channel is modified — eliminating the hue and saturation shifts inherent in RGB or YCbCr-domain grading.

The pipeline consists of four shader passes plus ancillary feature extraction, face detection, and black bar repaint. Three XGBoost models predict per-scene tone curve parameters from 88 GPU-extracted features, enabling fully automatic scene-adaptive grading with no user intervention.

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
  │
  ▼
  1. ps_ml_shadow.hlsl    — Dual-band detail enhancement (currently passthrough)
  2. ps_ml_highlight.hlsl — Highlight rolloff (passthrough — merged into gamma)
  3. ps_ml_gamma.hlsl     — Unified tone curve (shadow toe + gamma + highlight rolloff)
  4. ps_ml_chroma.hlsl    — Chroma vector tuning (Ct/Cp boost with skin protection)
  5. ps_ml_bars.hlsl      — Black bar repaint (letterbox haze fix)
```

All passes share a single constant buffer (`MLParams_t`, 280 bytes) updated once per frame. The debug passthrough mode (`MLDebugPassthrough`) binds a simple copy shader to all steps for bisecting shader vs plumbing issues.

## Tone Curve: The Unified Model

### Design Decision

Rather than three separate shader passes for shadow/gamma/highlight, a **single unified tone curve** operates on the normalized I channel in `ps_ml_gamma.hlsl`. This was a deliberate collapse: the three XGBoost models each predict a scalar that parameterizes one region of the same curve, and applying them sequentially in one pass avoids three round-trips through ICtCp conversion and texture ping-pong.

The highlight pass (`ps_ml_highlight.hlsl`) is now a pure passthrough. The shadow detail pass (`ps_ml_shadow.hlsl`) retains its dual-band DoG infrastructure for future use but is also passthrough.

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

The dual-band Difference-of-Gaussians (DoG) detail enhancement (`ps_ml_shadow.hlsl`) is architecturally complete but currently passthrough. The DoG computes frequency-separated detail at σ=1/σ=2 (shadow fine texture) and σ=2/σ=4 (midtone contrast recovery), with a soft-curve compressor to prevent halo artifacts. It was disabled due to gradient artifacts under investigation. The `shadowStrength` parameter is hardcoded to 0.0.

The four s3_shell sliders (Strength, Zone ceil, Skin protect, Ratio cap) for the SHADOW section have been removed. The CONTRAST section (same 4-slider pattern) and FACE DETECT section are also removed from the UI. When re-enabled, these should be re-added.

### Contrast Recovery — Disabled in UI

The dual-band contrast recovery (MCStrength, MCZoneLo, MCSkinProtect, MCRatioCap) is still wired in the cbuffer and shader but the UI section was removed. `contrastStrength` is driven by `m_bMLContrastEnabled` which s3_shell sets to false.

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

### DEBUG Section

| Control | Type | Key |
|---------|------|-----|
| Passthrough | Toggle | zion.debugCopyShaders |
| Frame log | Toggle | zion.debugFrameLog |
| Face boxes | Toggle | zion.faceDebugVis |

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
144     MCStrength          4       0.0 (contrast disabled)
148     MCZoneLo            4
152     MCSkinProtect       4
156     MCRatioCap          4
160     FaceProtect         4       0.8
164     FaceFeather         4       0.12
168     FaceExpand          4       2.0
172     FaceDebugVis        4       Toggle
176     NumFaces            4 (int) 0–4
180     _padFace[3]         12      Alignment
192     FaceBox[16]         64      4 × (x, y, w, h) UV
256     FaceTrackId[4]      16      ByteTrack persistent IDs
────────────────────────────────────────────────────────
Total: 272 bytes (+ padding to 16-byte alignment = 272)
```

## File Map

### Shaders (`Shaders/d3d11/`)

| File | Purpose | State |
|------|---------|-------|
| `ps_ml_luma.hlsl` | 256×144 luma downsample for feature extraction | Active |
| `ps_ml_face_down.hlsl` | 480×288 color downsample for face detection | Active |
| `ps_ml_gamma.hlsl` | Unified tone curve (shadow + gamma + highlight) + face debug vis | Active |
| `ps_ml_shadow.hlsl` | Dual-band DoG detail enhancement | Passthrough |
| `ps_ml_highlight.hlsl` | Highlight rolloff (merged into gamma) | Passthrough |
| `ps_ml_chroma.hlsl` | ICtCp Ct/Cp chroma boost with skin LUT protection | Active |
| `ps_ml_bars.hlsl` | Black bar repaint for projector haze | Active |

### C++ Source (`Source/`)

| File | Role |
|------|------|
| `DX11VideoProcessor.h` | `MLParams_t` cbuffer struct, ML state variables, XGBoost model instances |
| `DX11VideoProcessor.cpp` | `SetMLParams()` (model inference + parameter packing), `ExtractMLFeatures()`, render pipeline hookup |
| `face_detect.h` / `face_detect.cpp` | YuNet ONNX wrapper — init, preprocess, inference, NMS, sync detection |

### Training Scripts (`libplacebo/ml/`)

| File | Model | Target |
|------|-------|--------|
| `train_xgb_l2_power.py` | Gamma | `2048 / trim_2081_power` |
| `train_xgb_shadow_model.py` | Shadow toe | Blended L2 offset + shadow crush |
| `train_xgb_highlight_model.py` | Highlight rolloff | Gold vs spline knot deviation |
| `train_xgb_sat_gain.py` | Saturation gain | `trim_2081_sat_gain / 2048` (NOT deployed) |

### UI (`mpc-hc/tools/engine_s3/s3_shell.cpp`)

Controls panel with TONE CURVE, CHROMA, DEBUG, and BLACK BARS sections. Removed: SHADOW (DoG), CONTRAST, FACE DETECT sections. Shadow toe slider added to TONE CURVE alongside Gamma and Highlight.

## Current State (2026-09-08)

### Working

- Unified tone curve: three XGBoost models → single shader pass on ICtCp I channel
- Auto mode: per-scene gamma, shadow toe, and highlight rolloff prediction with IIR temporal smoothing
- Manual mode: all three tone curve parameters user-adjustable via s3_shell sliders
- ICtCp chroma boost with smooth monotonic curve and skin LUT protection
- Face detection via YuNet + ByteTrack with debug visualization
- Black bar repaint for projector haze
- Scene cut detection resets both gamma IIR filter and ByteTrack tracker

### Disabled / Passthrough

- DoG shadow detail enhancement (gradient artifacts under investigation)
- Contrast recovery band (UI removed, cbuffer wired)
- Hunt compensation (redundant in ICtCp)
- Face-guided gamma protection (infrastructure ready, not activated)
- Saturation gain model (per-title aesthetic, not scene-corrective)

### Build

- MPCVR: `MpcVideoRenderer.sln` → Release x64 via MSBuild
- s3_shell: `build_s3_shell.bat` via cl.exe
- Distribution: `_bin\MpcVideoRenderer-0.10.8.2601\` with Install/Uninstall scripts
- Dependencies: ONNX Runtime + DirectML DLLs, YuNet `.onnx` model, XGBoost `.plxgb` model files
