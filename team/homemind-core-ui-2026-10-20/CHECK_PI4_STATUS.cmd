@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\status.ps1"
set "result=%errorlevel%"
echo.
if "%result%"=="0" (
  echo Pi4 status check finished successfully.
) else (
  echo Pi4 status check failed. Confirm the LAN cable and Pi4 power.
)
pause
exit /b %result%
