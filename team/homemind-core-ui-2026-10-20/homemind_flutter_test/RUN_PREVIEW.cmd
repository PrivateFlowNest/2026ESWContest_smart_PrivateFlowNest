@echo off
setlocal
pushd "%~dp0"
for /f "usebackq delims=" %%P in (`wsl.exe -d Ubuntu -- wslpath -a "%CD%"`) do set "WSL_PROJECT=%%P"
echo HomeMind UI is building. Keep this window open after the build finishes.
wsl.exe -d Ubuntu -- bash -lc "cd '%WSL_PROJECT%' && ~/.local/opt/flutter/bin/flutter build web --release --pwa-strategy=none && echo Open http://localhost:8765 && python3 -m http.server 8765 --bind 0.0.0.0 --directory build/web"
popd
pause
