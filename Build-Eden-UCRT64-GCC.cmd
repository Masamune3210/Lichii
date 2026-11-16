@echo off
setlocal
cd /d "%~dp0"

set "MSYS_ROOT=C:\msys64"
if exist "%MSYS_ROOT%\msys2_shell.cmd" goto RunBuild

if exist "%LOCALAPPDATA%\Programs\msys64\msys2_shell.cmd" (
    set "MSYS_ROOT=%LOCALAPPDATA%\Programs\msys64"
    goto RunBuild
)

echo.
echo ERROR: MSYS2 was not found.
echo Run Setup-Eden-CI-MSYS2.ps1 first.
echo.
pause
exit /b 1

:RunBuild
call "%MSYS_ROOT%\msys2_shell.cmd" -defterm -here -no-start -ucrt64 -shell bash -c "./Build-Eden-UCRT64-GCC.sh"
set "EXITCODE=%ERRORLEVEL%"

echo.
if not "%EXITCODE%"=="0" echo Build script exited with code %EXITCODE%.
pause
exit /b %EXITCODE%
