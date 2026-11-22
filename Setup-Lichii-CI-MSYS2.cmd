@echo off
setlocal
cd /d "%~dp0"

where pwsh.exe >nul 2>&1
if errorlevel 1 goto WindowsPowerShell

pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-Lichii-CI-MSYS2.ps1"
set "EXITCODE=%ERRORLEVEL%"
goto Finished

:WindowsPowerShell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-Lichii-CI-MSYS2.ps1"
set "EXITCODE=%ERRORLEVEL%"

:Finished
echo.
if not "%EXITCODE%"=="0" echo Setup script exited with code %EXITCODE%.
pause
exit /b %EXITCODE%
