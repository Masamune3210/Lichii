@echo off
rem Default local Windows build: MSVC + native PDBs.
rem Fallback: Build-Lichii-UCRT64-GCC.cmd
call "%~dp0Build-Lichii-MSVC.cmd" %*
exit /b %ERRORLEVEL%
