@cd /d "%~dp0"
@regsvr32.exe "%~dp0/unplugged_vr.ax" /s
@if %errorlevel% NEQ 0 goto error
:success
@echo.
@echo.
@echo    Installation succeeded.
@echo.
@echo    Please do not delete the unplugged_vr.ax file.
@echo    The installer has not copied the files anywhere.
@echo.
@goto done
:error
@echo.
@echo.
@echo    Installation failed.
@echo.
@echo    You need to right click "Install_Unplugged_64.cmd" and choose "run as admin".
@echo    (A sharing violation here usually means the player is still running and
@echo    holding the .ax open - close it and try again.)
@echo.
:done
@pause >NUL
