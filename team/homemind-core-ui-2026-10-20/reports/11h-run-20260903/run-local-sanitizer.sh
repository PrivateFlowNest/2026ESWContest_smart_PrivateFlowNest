#!/bin/sh
set -eu

project_root=/mnt/c/Users/junwo/Documents/Codex/2026-08-26/pi-4-pi4-sql-lite
result_dir="$project_root/.tmp/asan-runtime"
broker_port=28888

mkdir -p "$result_dir"
rm -f "$result_dir/homemind.db" "$result_dir/broker.log" \
  "$result_dir/core.log" "$result_dir/selftest.log"
broker_pid=""
core_pid=""
cleanup() {
  if [ -n "$core_pid" ]; then
    kill "$core_pid" 2>/dev/null || true
    wait "$core_pid" 2>/dev/null || true
  fi
  if [ -n "$broker_pid" ]; then
    kill "$broker_pid" 2>/dev/null || true
    wait "$broker_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

mosquitto -p "$broker_port" >"$result_dir/broker.log" 2>&1 &
broker_pid=$!
sleep 1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$project_root/.tmp/asan/homemind-core" \
  --broker-host 127.0.0.1 --broker-port "$broker_port" \
  --database "$result_dir/homemind.db" \
  --sensor-rules "$project_root/contracts/sensor-rules.json" \
  --scenario-rules "$project_root/contracts/scenario-rules.json" \
  >"$result_dir/core.log" 2>&1 &
core_pid=$!
sleep 1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$project_root/.tmp/asan/homemind-selftest" \
  --broker-host 127.0.0.1 --broker-port "$broker_port" \
  --database "$result_dir/homemind.db" \
  --node-id CTRL-ESP32-ACT-01 | tee "$result_dir/selftest.log"

kill "$core_pid"
wait "$core_pid"
core_pid=""
if grep -Eq 'AddressSanitizer|runtime error:' "$result_dir/core.log" "$result_dir/selftest.log"; then
  echo SANITIZER_FINDING >&2
  exit 1
fi
echo LOCAL_SANITIZER_PASS
