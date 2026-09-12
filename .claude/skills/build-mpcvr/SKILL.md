---
name: build-mpcvr
description: Build the MPC Video Renderer (MpcVideoRenderer64.ax, Zion ML grading renderer). Use when asked to build/rebuild VideoRenderer output, fix a compile error, or produce the MPCVR .ax filter. x64 only.
---

# Build MPC Video Renderer (MPCVR)

Target artifact: `_bin\Filter_x64\MpcVideoRenderer64.ax`

This is the Zion ML grading renderer (branch `zion-ml-phase0`): D3D9/D3D11 video
renderer plus ONNX-Runtime face/sky/person inference and the ML grade pipeline.

## Environment prerequisites (verified on a fresh box + VS2022 17.14 + MSYS2 UCRT64)

- **git is not on PATH** by default (the box only has GitHub Desktop). The
  project's `update_revision.cmd` PreBuildEvent runs git, so the build env needs
  git's `cmd` dir on PATH:
  ```powershell
  $env:PATH = "$env:LOCALAPPDATA\GitHubDesktop\app-*\resources\app\git\cmd;" + $env:PATH
  ```
- **`cmd` will not resolve a bare `*.bat` from the CWD** (NoDefaultCurrentDirectoryInExePath
  set in this shell): always invoke scripts by full path.
- MSVC toolset + Windows SDK come from VS2022 (check with vswhere). The sln
  self-detects via `common.props` (VisualStudioVersion → v143).

## Required sibling dependency — ByteTrack

`Source\MpcVideoRenderer.vcxproj` hard-codes the ByteTrack multi-object tracker at
`C:\Code\byte-track-eigen` (include dirs + 8 `ClCompile` entries), **and the
rendered `BYTETracker.h` was never committed to the repo** — it lives as a
modified file in that sibling clone (adds `get_recent_lost_tracks`).

- Keep the `byte-track-eigen` clone present at `C:\code\byte-track-eigen` (Windows
  paths are case-insensitive, so `C:\Code\...` in the vcxproj resolves to it).
- It needs its `external\eigen` checked out (Eigen headers).
- Do not remove the modified state of `include/BYTETracker.h` / `src/BYTETracker.cpp`.

## Build (x64 only!)

Build **x64 Release only**. The vendored `Source\onnxrt\onnxruntime.lib` is an
x64-only import lib — an x86 build fails at link with
`unresolved external symbol _OrtGetApiBase@0` (decorated x86 names). The DeckLink
pipeline is x64 anyway.

```powershell
Push-Location 'C:\code\unplugged-VideoRenderer'
MSBuild.exe MpcVideoRenderer.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /verbosity:minimal
Pop-Location
```

(`build_mpcvr.cmd Release NoWait` also works but always builds x86 **and** x64,
and the x86 leg fails on the x64-only onnxruntime.lib — prefer direct MSBuild.)

## Verify

```powershell
Get-Item 'C:\code\unplugged-VideoRenderer\_bin\Filter_x64\MpcVideoRenderer64.ax'
# Fresh LastWriteTime, ~1.6 MB
```

Shaders compile during the build into `_bin\shaders\*.cso`.
Outputs also land in `_bin\MpcVideoRenderer-<ver>\` (distributable filter folder
with Install/Uninstall .cmd scripts) — that's the folder to copy for registration
(`regsvr32 MpcVideoRenderer64.ax` / the Install_MPCVR_64.cmd).

## Register for runtime use

`s3_shell.exe` finds MPCVR by CLSID at run time, so the filter must be registered
(elevated):
```
cd _bin\MpcVideoRenderer-<ver> && Install_MPCVR_64.cmd   (right-click → Run as admin)
```
or `regsvr32 /s MpcVideoRenderer64.ax`.