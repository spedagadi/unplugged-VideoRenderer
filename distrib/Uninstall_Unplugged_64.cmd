@cd /d "%~dp0"
@regsvr32.exe "%~dp0/unplugged_vr.ax" /u /s
@echo.
@echo    Unregistered unplugged_vr.ax.
@echo    Renderer settings under HKCU are kept - delete them only if you mean to.
@pause >NUL
