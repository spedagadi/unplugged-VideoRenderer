@ECHO OFF
PUSHD "%~dp0"
REM This shell sets NoDefaultCurrentDirectoryInExePath, so cmd will not resolve a
REM bare command from the CWD. Clearing it is what the engine build script does.
SET "NoDefaultCurrentDirectoryInExePath="
CALL "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\vsdevcmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
cl /nologo /EHsc /O2 /std:c++17 /W3 /I C:\htpc gt7_probe.cpp /Fe:gt7_probe.exe
IF %ERRORLEVEL% NEQ 0 (ECHO COMPILE_FAILED=%ERRORLEVEL% & POPD & EXIT /B 1)
REM Tee into a file so the run can be re-read afterwards (the CSV is written by
REM the program itself; this captures the human-readable tables).
gt7_probe.exe > gt7_probe.txt 2>&1
ECHO PROBE_EXIT=%ERRORLEVEL%
TYPE gt7_probe.txt
POPD
