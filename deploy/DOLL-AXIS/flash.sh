#!/usr/bin/env bash
# Build and flash the one-axis doll controller from the PC over USB. RUN ONLY AFTER THE USER APPROVES
# (board chosen, pins/mechanics set in firmware/include/doll_axis_config.h).
#   bash deploy/DOLL-AXIS/flash.sh teensy            # Teensy 4.1 (Teensy Loader; press the button if asked)
#   bash deploy/DOLL-AXIS/flash.sh esp32 COM7        # ESP32 DevKit
# Then drive it:  python firmware/tools/doll_axis_cli.py --port <port> HOME "MOVE bed"
set -euo pipefail
board="${1:?teensy or esp32}"; port="${2:-}"
fw="$(cd "$(dirname "$0")/../../firmware" && pwd)"
py="$fw/.venv-pio/Scripts/python"; [[ -x "$py" ]] || py="$fw/.venv-pio/bin/python"
export PYTHONUTF8=1
cd "$fw"
"$py" -m platformio test -e native          # motion logic on the PC first (simulated carriage)
case "$board" in
  teensy) "$py" -m platformio run -e teensy41_axis -t upload;;
  esp32)  [[ -n "$port" ]] || { echo "serial port required for esp32" >&2; exit 2; }
          "$py" -m platformio run -e esp32_axis -t upload --upload-port "$port";;
  *) echo "board must be teensy or esp32" >&2; exit 2;;
esac
