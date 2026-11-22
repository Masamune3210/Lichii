@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL="
if not exist "%VSWHERE%" goto NoVisualStudio

for /f "delims=" %%I in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS_INSTALL=%%I"
if not defined VS_INSTALL goto NoVisualStudio

if not exist "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" goto NoVisualStudio
call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=amd64 -host_arch=amd64
if errorlevel 1 goto SetupFailed

where pwsh.exe >nul 2>&1
if not errorlevel 1 (
    pwsh.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Eden-MSVC.ps1" %*
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Eden-MSVC.ps1" %*
)
set "RESULT=%ERRORLEVEL%"
goto Finish

:NoVisualStudio
echo ERROR: No Visual Studio installation with x64 MSVC tools was found.
echo Install the Desktop development with C++ workload.
set "RESULT=1"
goto Finish

:SetupFailed
echo ERROR: Failed to initialize the x64 MSVC developer environment.
set "RESULT=1"

:Finish
echo.
if not "%RESULT%"=="0" echo MSVC build exited with code %RESULT%.
if not defined EDEN_NO_PAUSE pause
exit /b %RESULT%
