@ECHO OFF
SET "NoDefaultCurrentDirectoryInExePath="
CALL "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\vsdevcmd.bat" -arch=amd64 -host_arch=amd64 -no_logo >nul
cd /d C:\Code\VideoRenderer
cl /nologo /EHsc /W3 -I Source test_ml_loader.cpp Source\ml_model.cpp /Fe:test_ml_loader.exe
ECHO BUILD_EXIT=%ERRORLEVEL%
