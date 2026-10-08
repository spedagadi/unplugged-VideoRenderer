@ECHO OFF
REM PUSHD to the script's own directory: msbuild is given RELATIVE project paths, so
REM invoking this batch from anywhere else silently builds nothing (MSB1009) -- and a
REM caller that only checks the .ax timestamp can be fooled by the previous build
REM still sitting in _bin. Same idiom as build_s3engine.bat and rebuild_engine.bat.
PUSHD "%~dp0"
SET "NoDefaultCurrentDirectoryInExePath="
CALL "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\vsdevcmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
msbuild MpcVideoRenderer.sln /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal
ECHO MPCVR_EXIT=%ERRORLEVEL%
POPD
