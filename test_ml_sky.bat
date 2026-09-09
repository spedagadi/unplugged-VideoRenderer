@ECHO OFF
SET "NoDefaultCurrentDirectoryInExePath="
CALL "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\vsdevcmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
cd /d C:\Code\VideoRenderer
cl /nologo /EHsc /W4 /std:c++17 -I Source test_ml_sky.cpp /link Source\onnxrt\onnxruntime.lib delayimp.lib /DELAYLOAD:onnxruntime.dll
ECHO BUILD_EXIT=%ERRORLEVEL%