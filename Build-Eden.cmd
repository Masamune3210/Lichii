@echo off
rem Default local Windows build: MSVC + native PDBs.
rem Fallback: Build-Eden-UCRT64-GCC.cmd
call "%~dp0Build-Eden-MSVC.cmd" %*
exit /b %ERRORLEVEL%
