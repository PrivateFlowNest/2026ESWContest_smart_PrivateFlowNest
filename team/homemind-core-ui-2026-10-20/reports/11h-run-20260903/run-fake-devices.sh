#!/bin/sh
set -eu

result_dir=/data/share/homemind/testing-11h/final-regression-devices
library_path=/data/share/homemind/lib
simulator=/data/share/homemind/bin/homemind-simulator
duration_seconds=${1:-120}

mkdir -p "$result_dir"
actuator_pid=""
lighting_pid=""
cleanup() {
  if [ -n "$actuator_pid" ]; then
    kill "$actuator_pid" 2>/dev/null || true
    wait "$actuator_pid" 2>/dev/null || true
  fi
  if [ -n "$lighting_pid" ]; then
    kill "$lighting_pid" 2>/dev/null || true
    wait "$lighting_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

LD_LIBRARY_PATH="$library_path" "$simulator" \
  --broker-host 10.10.10.2 \
  --broker-port 1883 \
  --node-id CTRL-ESP32-ACT-01 \
  --profile normal \
  --duration-seconds "$duration_seconds" \
  --interval-ms 1000 >"$result_dir/actuator.log" 2>&1 &
actuator_pid=$!

LD_LIBRARY_PATH="$library_path" "$simulator" \
  --broker-host 10.10.10.2 \
  --broker-port 1883 \
  --node-id CTRL-ESP8266-LED-01 \
  --profile normal \
  --duration-seconds "$duration_seconds" \
  --interval-ms 1000 >"$result_dir/lighting.log" 2>&1 &
lighting_pid=$!

sleep 2
kill -0 "$actuator_pid"
kill -0 "$lighting_pid"
echo "FAKE_DEVICES_READY"
wait "$actuator_pid"
wait "$lighting_pid"
echo "FAKE_DEVICES_FINISHED"
