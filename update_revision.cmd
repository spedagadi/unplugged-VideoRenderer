@ECHO OFF
SETLOCAL
CD /D %~dp0

REM A revision.h that already carries REV_NUM is hand-maintained and must be kept.
REM Clobbering it with the stub below costs a full build: make_dist.cmd then aborts
REM the post-build step with "unable to read version from Include\Version.h /
REM revision.h" and MSBuild reports failure even though the .ax compiled and
REM linked correctly — which reads as a broken build when nothing is wrong with it.
IF EXIST revision.h (
  FINDSTR /I /L /C:"define REV_NUM" revision.h >nul 2>&1 && GOTO :END
)

ECHO #pragma once > revision.h

SET gitexe="git.exe"
%gitexe% --version
IF /I %ERRORLEVEL%==0 GOTO :GitOK

SET gitexe="c:\Program Files\Git\cmd\git.exe"
IF NOT EXIST %gitexe% set gitexe="c:\Program Files\Git\bin\git.exe"
REM git also ships inside GitHub Desktop, which is the only copy on this machine.
IF NOT EXIST %gitexe% set gitexe="%LOCALAPPDATA%\GitHubDesktop\app-3.6.4\resources\app\git\cmd\git.exe"
IF NOT EXIST %gitexe% GOTO :END

:GitOK

%gitexe% log -1 --date=format:%%Y.%%m.%%d --pretty=format:"#define REV_DATE %%ad%%n" >> revision.h
%gitexe% log -1 --pretty=format:"#define REV_HASH %%h%%n" >> revision.h

<nul set /p strTemp=#define REV_BRANCH >> revision.h
%gitexe% symbolic-ref --short HEAD >> revision.h
IF %ERRORLEVEL% NEQ 0 (
ECHO LOCAL >> revision.h
)

<nul set /p strTemp=#define REV_NUM >> revision.h
%gitexe% rev-list --count HEAD >> revision.h
IF %ERRORLEVEL% NEQ 0 (
ECHO 0 >> revision.h
)

:END
ENDLOCAL
EXIT /B
