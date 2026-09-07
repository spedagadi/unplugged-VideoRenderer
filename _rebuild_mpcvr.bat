@ECHO OFF
SET "NoDefaultCurrentDirectoryInExePath="
CALL "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\vsdevcmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
msbuild MpcVideoRenderer.sln /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal
ECHO MPCVR_EXIT=%ERRORLEVEL%
