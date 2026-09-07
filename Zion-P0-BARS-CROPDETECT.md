# Zion P0 — Black Bars Detection & Repaint

## Purpose
Detect letterbox black bars on a 256x144 R16_UNORM luma readback texture and
repaint them to display-black for improved perceived contrast on projectors.

## Architecture

### Detection pipeline (`ml_features.h` / `ml_features.cpp`)
- **Input**: 256x144 R16_UNORM readback (luma only, post-tonemap)
- **Row scan**: per-row mean luma, threshold at `kCropLimit = 0.01f`
- **Output**: `MLBarBounds { contentTop, contentBottom, threshold }`
- **Temporal union**: 90-frame sliding window; 2-row arming threshold;
  1-row guard margin subtracted from detected bounds to avoid painting
  into active image area

### Render step (`ps_ml_bars.hlsl`)
- Constant-buffer driven: `uBarTop`, `uBarBot`, `uBarV0`, `uBarSpan`,
  `uBarFeather`, `uBarBlack`, `uBarHL`
- Normal mode: paints bars to `uBarBlack` (display black, 0.0)
- Highlight mode: paints bars red (debug overlay)
- Feathered edge (2-texel) to avoid hard seam at bar boundary

### Control flow
```
s3_shell click
  -> ApplyRow() [logged: row, key, type, old->new]
  -> BuildMlFromRows() [logged: all MlSettings + configSeq]
  -> PushMlToRenderer()
    -> MpcPlayer::SetMlSettings() [logged: configSeq, hr, sizeof]
      -> ApplyMlToSettings() [logged: all fields + configSeq via OutputDebugString]
      -> IVideoRenderer::SetSettings()
        -> CDX11VideoProcessor::Configure() [logged: configSeq + all ML fields via DLog]
          -> UpdateMLActive()
```

### s3_shell controls
| Control | Key | Type | Default |
|---------|-----|------|---------|
| Repaint bars | `zion.barsRepaint` | Toggle | ON |
| Highlight bars | `zion.barsHighlight` | Toggle | OFF |

### Diagnostic logging
- **s3_shell**: `s3_shell_diag.log` (same dir as exe), timestamped `HH:MM:SS.mmm`
  - `[CLICK]` every button press with old/new state
  - `[BUILD]` every MlSettings assembly with all fields + configSeq
  - `[PUSH]` SetMlSettings call + HRESULT
  - `[ECHO]` readback verification from MPCVR
- **Engine**: `OutputDebugStringA` (visible in DebugView)
  - `[ENGINE]` ApplyMlToSettings with all fields + configSeq + sizeof(Settings_t)
  - `[ENGINE]` SetMlSettings entry/exit with null-renderer checks
- **MPCVR**: `DLog` (debug output stream)
  - Configure() logs configSeq + every ML field + sizeof(Settings_t)
  - Per-frame: BARS_CBUF, BARS_UNION, BARS_DRAW in ml_debug.csv

### Shared header contract
Both s3_shell (via mpchc_engine) and MPCVR compile against the same
`IVideoRenderer.h` (`Source/IVideoRenderer.h`). The engine's vcxproj gets
it via `/p:ML_MPCVR_INCLUDE=C:\Code\VideoRenderer\Source`. Both binaries
must be rebuilt when Settings_t changes or struct offsets diverge at runtime.

`configSeq` (uint32_t, end of Settings_t) is a monotonic counter incremented
by s3_shell on every config push, enabling cross-binary log correlation.

### Build order (mandatory after Settings_t changes)
1. Compile shaders (if HLSL changed): `compile_shaders.cmd`
2. Build MPCVR: `msbuild MpcVideoRenderer.sln /p:Configuration=Release /p:Platform=x64`
3. Register DLL: `regsvr32 /s MpcVideoRenderer64.ax` (elevated)
4. Build engine: `rebuild_engine.bat`
5. Build s3_shell: `build_s3_shell.bat`

### Settings persistence
MPCVR persists all ML settings (including bars) to the Windows registry.
s3_shell pulls from MPCVR on file open (`PullMlFromRenderer`) so the UI
reflects the last-used state. No separate s3_shell config file.

## Changes in this commit

### Removed: Contrast Recovery (CR) and Resonance/Radiance
- Deleted `ps_ml_cr.hlsl`, `ps_ml_radiance.hlsl` and their .cso resources
- Removed from Shaders.vcxproj, compile_shaders.cmd, resource.h, MpcVideoRenderer.rc2
- Removed CR/Radi fields from Settings_t, DX11VideoProcessor (member vars,
  shader pointers, GetPostScaleSteps, UpdateMLActive, SetMLParams, Process,
  Configure, GetMLStats), VideoRenderer.cpp (registry), PropPage.cpp (UI)
- HLSL cbuffer slots renamed to `_reserved0..5` to preserve offset stability
- MLParams_t: replaced CR/Radi floats with `float _reserved[6]`

### Added: P0 black bars
- `ml_features.h` / `ml_features.cpp`: bar detection with temporal union
- `ps_ml_bars.hlsl`: bar repaint/highlight shader
- `DX11VideoProcessor`: bar render step in Process(), UpdateMLBarsFromDetected(),
  bar cbuffer upload, `!m_bMLDebugCopy` passthrough gate
- `IVideoRenderer.h`: `bMLBars`, `bMLBarsHighlight`, `configSeq` fields
- `MpcPlayer.h/.cpp`: `barsEnabled`, `barsHighlight`, `configSeq` in MlSettings
- `s3_shell.cpp`: Repaint bars / Highlight bars toggles, diagnostic log system,
  settings pull from MPCVR on file open

### Added: end-to-end diagnostic logging
- `configSeq` (uint32_t) in Settings_t for cross-binary correlation
- s3_shell: `s3_shell_diag.log` with timestamped CLICK/BUILD/PUSH/ECHO traces
- Engine: OutputDebugStringA in ApplyMlToSettings and SetMlSettings
- MPCVR: expanded DLog in Configure() with configSeq and all ML fields

### Fixed: s3_shell startup gate
- `g_mlMasterEnable` now synced from `zion.enabled` row state in BuildRows()
- Removed hardcoded "demo defaults" override; UI trusts MPCVR's persisted settings
- Added PullMl/Hdr/OutFromRenderer after file-picker open
