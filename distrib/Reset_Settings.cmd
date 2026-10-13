@echo off
echo.
echo.
title Restore Unplugged Renderer default settings...
rem Our own key only. The upstream filter's key is deliberately NOT touched: it
rem belongs to a different product now, and a reset here must not reach into it.
rem
rem The migration marker is left in place on purpose. If it were deleted too, the next
rem start would re-import the legacy values and this script would look like it had
rem done nothing.
start /min reg delete "HKEY_CURRENT_USER\Software\Unplugged\Renderer" /f
echo    settings were reset to default
echo.
pause >NUL
