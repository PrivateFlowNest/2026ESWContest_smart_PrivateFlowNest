#!/usr/bin/env bash
# Build and flash CTRL-ESP32-ACT-01 from the PC over USB. RUN ONLY AFTER THE USER APPROVES (board connected, secrets.h filled).
#   bash deploy/CTRL-ESP32-ACT-01/flash.sh COM5          (Windows port)  |  /dev/ttyUSB0 (Linux)  |  /dev/cu.usbserial-XXXX (macOS)
# PlatformIO (once):  python3 -m venv firmware/.venv-pio && firmware/.venv-pio/bin/python -m pip install platformio==6.1.16
# Then watch the serial log:  firmware/.venv-pio/Scripts/python -m platformio device monitor -p <port> -b 115200
set -euo pipefail
port="${1:?serial port, e.g. COM5}"
fw="$(cd "$(dirname "$0")/../../firmware" && pwd)"
[[ -f "$fw/include/secrets.h" ]] || { echo "firmware/include/secrets.h missing: copy secrets.h.example and fill it in locally (never commit)" >&2; exit 2; }
py="$fw/.venv-pio/Scripts/python"; [[ -x "$py" ]] || py="$fw/.venv-pio/bin/python"
[[ -x "$py" ]] || { echo "PlatformIO venv missing: see the PlatformIO line at the top of this script" >&2; exit 2; }
export PYTHONUTF8=1
cd "$fw"
# Logic tests first. The native env pins the Windows MinGW toolchain, so they run on the Windows PC only
# (they passed there: firmware/README.md); a macOS/Linux laptop goes straight to the board build.
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) "$py" -m platformio test -e native ;;
  *) echo "  native logic tests: Windows PC only (passed there), skipped on $(uname -s)" ;;
esac
"$py" -m platformio run -e esp32dev -t upload --upload-port "$port"
