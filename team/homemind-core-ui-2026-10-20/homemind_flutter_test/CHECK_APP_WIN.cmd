@echo off
rem Windows Flutter check: device catalog, pub get, analyze, test.
rem Uses FLUTTER if set, else %USERPROFILE%\dev\flutter, else flutter on PATH.
setlocal EnableExtensions
pushd "%~dp0"

if not defined FLUTTER if exist "%USERPROFILE%\dev\flutter\bin\flutter.bat" set "FLUTTER=%USERPROFILE%\dev\flutter\bin\flutter.bat"
if not defined FLUTTER for /f "delims=" %%F in ('where flutter.bat 2^>nul') do if not defined FLUTTER set "FLUTTER=%%F"
if not defined FLUTTER (
  echo Flutter was not found. Install it at %%USERPROFILE%%\dev\flutter or set FLUTTER.
  goto :fail
)

python tool\check_device_catalog.py
if errorlevel 1 goto :fail
call "%FLUTTER%" pub get
if errorlevel 1 goto :fail
call "%FLUTTER%" analyze
if errorlevel 1 goto :fail
call "%FLUTTER%" test
if errorlevel 1 goto :fail
popd
exit /b 0

:fail
popd
pause
exit /b 1
