#!/usr/bin/env bash
# PC check of the Octopus Klipper configs without hardware (Klipper batch mode, simulation only).
#   docker build -t homemind-klipper-check -f octopus/klipper/test/klipper-check.Dockerfile octopus/klipper/test
#   docker run --rm -v "<repo>/octopus/klipper:/cfg" homemind-klipper-check bash /cfg/test/run-batch-check.sh
set -euo pipefail
cd /klipper
status=0
for cfg in /cfg/printer-cartesian.cfg /cfg/printer-manual-stepper.cfg; do
  name="$(basename "$cfg" .cfg)"
  gcode="/cfg/test/${name}.gcode"
  echo "== $name"
  if /venv/bin/python klippy/klippy.py "$cfg" -i "$gcode" -o /tmp/"$name".serial -v -d out/klipper.dict \
      > /tmp/"$name".log 2>&1; then
    echo "BATCH_OK $name ($(wc -c < /tmp/"$name".serial) bytes of MCU commands)"
    # Ignore container noise (no device-tree/DMI files, reactor timing); anything else fails the check.
    problems="$(grep -E "WARNING|Error|error" /tmp/"$name".log | grep -vE "device-tree/model|dmi/id/product_name|Reactor .* busy" || true)"
    if [[ -n "$problems" ]]; then status=1; echo "PROBLEMS in $name:"; echo "$problems" | sort | uniq -c | head; fi
  else
    status=1
    echo "BATCH_FAIL $name"
    tail -30 /tmp/"$name".log
  fi
done
exit $status
