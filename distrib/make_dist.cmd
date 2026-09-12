@ECHO OFF
REM (C) 2026 see Authors.txt
REM
REM This file is part of MPC-BE.
REM
REM MPC-BE is free software; you can redistribute it and/or modify
REM it under the terms of the GNU General Public License as published by
REM the Free Software Foundation; either version 3 of the License, or
REM (at your option) any later version.
REM
REM MPC-BE is distributed in the hope that it will be useful,
REM but WITHOUT ANY WARRANTY; without even the implied warranty of
REM MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
REM GNU General Public License for more details.
REM
REM You should have received a copy of the GNU General Public License
REM along with this program.  If not, see <http://www.gnu.org/licenses/>.
REM
REM ---------------------------------------------------------------------------
REM Assembling the distributable filter folder (same layout as an official
REM release, e.g. "MpcVideoRenderer-0.10.7.2560") into _bin inside this repo.
REM Run after a Release build, or call it as a post-build step. Then just right
REM click Install_MPCVR_64.cmd / Install_MPCVR_32.cmd and choose "Run as
REM administrator" to register, or Uninstall_MPCVR_*.cmd to deregister.
REM ---------------------------------------------------------------------------

SETLOCAL ENABLEDELAYEDEXPANSION
SET "FAILED=0"

CD /D "%~dp0\.."

SET "VERMAJOR="
SET "VERMINOR="
SET "VERBUILD="
FOR /F "tokens=3 delims= " %%A IN ('FINDSTR /I /L /C:"define VER_MAJOR" "Include\Version.h"') DO SET "VERMAJOR=%%A"
FOR /F "tokens=3 delims= " %%A IN ('FINDSTR /I /L /C:"define VER_MINOR" "Include\Version.h"') DO SET "VERMINOR=%%A"
FOR /F "tokens=3 delims= " %%A IN ('FINDSTR /I /L /C:"define VER_BUILD" "Include\Version.h"') DO SET "VERBUILD=%%A"
FOR /F "tokens=3 delims= " %%A IN ('FINDSTR /I /L /C:"define REV_NUM" "revision.h"') DO SET "REVNUM=%%A"

IF NOT DEFINED VERMAJOR GOTO nover
IF NOT DEFINED VERMINOR GOTO nover
IF NOT DEFINED VERBUILD GOTO nover
IF NOT DEFINED REVNUM GOTO nover

SET "DIST_DIR=_bin\MpcVideoRenderer-%VERMAJOR%.%VERMINOR%.%VERBUILD%.%REVNUM%"
IF NOT EXIST "%DIST_DIR%\" MKDIR "%DIST_DIR%"

IF EXIST "_bin\Filter_x64\MpcVideoRenderer64.ax" (
  COPY /Y "_bin\Filter_x64\MpcVideoRenderer64.ax" "%DIST_DIR%\" >NUL
  IF ERRORLEVEL 1 SET "FAILED=1"
) ELSE (
  ECHO [WARN] _bin\Filter_x64\MpcVideoRenderer64.ax not found - build Release x64 first.
)
IF EXIST "_bin\Filter_x86\MpcVideoRenderer.ax" (
  COPY /Y "_bin\Filter_x86\MpcVideoRenderer.ax" "%DIST_DIR%\" >NUL
  IF ERRORLEVEL 1 SET "FAILED=1"
) ELSE (
  ECHO [WARN] _bin\Filter_x86\MpcVideoRenderer.ax not found - build Release x86 first.
)

COPY /Y "distrib\Install_MPCVR_32.cmd"         "%DIST_DIR%\" >NUL
COPY /Y "distrib\Install_MPCVR_64.cmd"         "%DIST_DIR%\" >NUL
COPY /Y "distrib\Uninstall_MPCVR_32.cmd"       "%DIST_DIR%\" >NUL
COPY /Y "distrib\Uninstall_MPCVR_64.cmd"       "%DIST_DIR%\" >NUL
COPY /Y "distrib\Reset_Settings.cmd"           "%DIST_DIR%\" >NUL
COPY /Y "LICENSE.txt"                          "%DIST_DIR%\" >NUL
COPY /Y "Readme.md"                            "%DIST_DIR%\" >NUL
COPY /Y "history.txt"                          "%DIST_DIR%\" >NUL

IF EXIST "Source\onnxrt\xgb_gamma_model.plxgb" (
  COPY /Y "Source\onnxrt\xgb_gamma_model.plxgb" "%DIST_DIR%\" >NUL
  COPY /Y "Source\onnxrt\xgb_shadow_recovery.plxgb" "%DIST_DIR%\" >NUL
  COPY /Y "Source\onnxrt\xgb_highlight_rolloff.plxgb" "%DIST_DIR%\" >NUL
) ELSE (
  ECHO [WARN] Source\onnxrt\*.plxgb not found - AUTO mode will be unavailable.
)

IF EXIST "Source\onnxrt\onnxruntime.dll" (
  COPY /Y "Source\onnxrt\onnxruntime.dll" "%DIST_DIR%\" >NUL
) ELSE (
  ECHO [WARN] onnxruntime.dll not found - ML segmentation will be unavailable.
)
REM face_detection_yunet.onnx disabled — person masks replace YuNet.
REM IF EXIST "Source\onnxrt\face_detection_yunet.onnx" (
REM   COPY /Y "Source\onnxrt\face_detection_yunet.onnx" "%DIST_DIR%\" >NUL
REM )
IF EXIST "Source\onnxrt\yolo26n-sem.onnx" (
  COPY /Y "Source\onnxrt\yolo26n-sem.onnx" "%DIST_DIR%\" >NUL
)
IF EXIST "Source\onnxrt\yolo26n-seg_op21.onnx" (
  COPY /Y "Source\onnxrt\yolo26n-seg_op21.onnx" "%DIST_DIR%\" >NUL
) ELSE (
  ECHO [WARN] yolo26n-seg_op21.onnx not found - person mask unavailable.
)

ECHO.
ECHO Distribution folder ready:  "%CD%\%DIST_DIR%"
ECHO.
ECHO   Register x64:   right click "%DIST_DIR%\Install_MPCVR_64.cmd"   -^> Run as administrator
ECHO   Deregister x64: right click "%DIST_DIR%\Uninstall_MPCVR_64.cmd" -^> Run as administrator
ECHO   Register x86:   right click "%DIST_DIR%\Install_MPCVR_32.cmd"   -^> Run as administrator
ECHO   Deregister x86: right click "%DIST_DIR%\Uninstall_MPCVR_32.cmd" -^> Run as administrator
ECHO.
EXIT /B %FAILED%

:nover
ECHO ERROR: unable to read version from Include\Version.h / revision.h.
EXIT /B 1