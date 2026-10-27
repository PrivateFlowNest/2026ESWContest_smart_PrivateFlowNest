@echo off
setlocal
pushd "%~dp0"
rem Original path: WSL distro "Ubuntu" with Flutter at ~/.local/opt/flutter.
rem When that is missing, fall back to the Windows Flutter (CHECK_APP_WIN.cmd).
wsl.exe -d Ubuntu -- bash -lc "test -x ~/.local/opt/flutter/bin/flutter" >nul 2>&1
if not "%errorlevel%"=="0" goto :windows
for /f "usebackq delims=" %%P in (`wsl.exe -d Ubuntu -- wslpath -a "%CD%"`) do set "WSL_PROJECT=%%P"
wsl.exe -d Ubuntu -- bash -lc "cd '%WSL_PROJECT%' && python3 tool/check_device_catalog.py && ~/.local/opt/flutter/bin/flutter pub get && ~/.local/opt/flutter/bin/flutter analyze && ~/.local/opt/flutter/bin/flutter test"
set "result=%errorlevel%"
popd
if not "%result%"=="0" pause
exit /b %result%

:windows
popd
call "%~dp0CHECK_APP_WIN.cmd"
exit /b %errorlevel%
