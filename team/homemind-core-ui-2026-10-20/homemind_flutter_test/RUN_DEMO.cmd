@echo off
rem HomeMind demo UI for the 7-inch 1024x600 display (Windows Flutter, web build).
rem
rem   RUN_DEMO.cmd                        broker ws://127.0.0.1:9001 (PC rehearsal)
rem   RUN_DEMO.cmd ws://10.10.10.2:9001   team Pi4 broker (direct LAN)
rem   RUN_DEMO.cmd mock                   no broker: virtual data with sample tasks
rem
rem Optional environment variables:
rem   HOMEMIND_BROKER_URL=ws://...        default broker when no argument is given
rem   HOMEMIND_KIOSK_BROWSER=1            open the browser full screen (--kiosk)
rem   HOMEMIND_SKIP_BUILD=1               reuse build\web from the last run
rem   HOMEMIND_NO_BROWSER=1               only serve; open the address yourself
rem   FLUTTER=C:\path\to\flutter.bat      Flutter to use
rem
rem Only the broker URL is passed. MQTT passwords are never put in the URL or build.
setlocal EnableExtensions
pushd "%~dp0"

set "BROKER=%~1"
if "%BROKER%"=="" set "BROKER=%HOMEMIND_BROKER_URL%"
if "%BROKER%"=="" set "BROKER=ws://127.0.0.1:9001"
set "PORT=8765"

if not defined FLUTTER if exist "%USERPROFILE%\dev\flutter\bin\flutter.bat" set "FLUTTER=%USERPROFILE%\dev\flutter\bin\flutter.bat"
if not defined FLUTTER for /f "delims=" %%F in ('where flutter.bat 2^>nul') do if not defined FLUTTER set "FLUTTER=%%F"
if not defined FLUTTER (
  echo Flutter was not found. Install it at %%USERPROFILE%%\dev\flutter or set FLUTTER.
  goto :fail
)

set "PYTHON="
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON set "PYTHON=%%P"
if not defined PYTHON for /f "delims=" %%P in ('where py 2^>nul') do if not defined PYTHON set "PYTHON=%%P"
if not defined PYTHON (
  echo Python was not found. It is needed for the local web server.
  goto :fail
)

if /i "%BROKER%"=="mock" (
  set "DEFINES=--dart-define=HOMEMIND_DATA_MODE=mock --dart-define=HOMEMIND_DEMO_SEED=true --dart-define=HOMEMIND_KIOSK=true"
  set "HM_URL=http://127.0.0.1:%PORT%/?mode=mock&seed=1&kiosk=1"
) else (
  set "DEFINES=--dart-define=HOMEMIND_MQTT_WS_URL=%BROKER% --dart-define=HOMEMIND_KIOSK=true"
  set "HM_URL=http://127.0.0.1:%PORT%/?broker=%BROKER%&kiosk=1"
)

if "%HOMEMIND_SKIP_BUILD%"=="1" if exist "build\web\index.html" goto :serve
echo Building HomeMind web UI (broker: %BROKER%) ...
call "%FLUTTER%" build web --release --no-web-resources-cdn --pwa-strategy=none %DEFINES%
if errorlevel 1 goto :fail

:serve
set "HM_BROWSER="
if "%HOMEMIND_NO_BROWSER%"=="1" goto :browser_done
for %%B in ("%ProgramFiles%\Google\Chrome\Application\chrome.exe" "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe" "%LocalAppData%\Google\Chrome\Application\chrome.exe" "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe") do (
  if not defined HM_BROWSER if exist %%B set "HM_BROWSER=%%~B"
)
if not defined HM_BROWSER goto :browser_done
set "HM_WINDOW=--window-size=1024,600 --window-position=0,0"
if "%HOMEMIND_KIOSK_BROWSER%"=="1" set "HM_WINDOW=--kiosk"
set "HM_ARGS=--user-data-dir=%TEMP%\homemind-demo-browser --no-first-run --app=%HM_URL% %HM_WINDOW%"

rem Opens the browser 3 seconds later, after the server below has started.
start "" /b powershell -NoProfile -Command "Start-Sleep -Seconds 3; Start-Process -FilePath $env:HM_BROWSER -ArgumentList $env:HM_ARGS"
:browser_done
if not defined HM_ARGS echo Browser not opened. Open the address below yourself (1024x600 window or --kiosk).
echo.
echo HomeMind demo UI: "%HM_URL%"
echo Press Ctrl+C in this window to stop the server.
echo.
"%PYTHON%" -m http.server %PORT% --bind 127.0.0.1 --directory build\web
popd
exit /b 0

:fail
popd
pause
exit /b 1
