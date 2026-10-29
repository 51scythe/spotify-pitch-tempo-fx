@echo off
setlocal
title Remove Spotify Pitch Tempo FX
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" -Action Remove
set "result=%errorlevel%"
echo.
pause
exit /b %result%
