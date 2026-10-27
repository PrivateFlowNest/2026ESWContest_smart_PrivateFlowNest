#!/bin/sh
set -eu

result_dir=/data/share/homemind/testing-11h/core-052-regression-01
broker_port=28884
library_path=/data/share/homemind/lib
binary_path=/data/share/homemind/testing-11h/bin

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

LD_LIBRARY_PATH="$library_path" /data/share/homemind/bin/mosquitto.real \
  -p "$broker_port" >"$result_dir/broker.log" 2>&1 &
broker_pid=$!
sleep 2

chmod 755 "$binary_path/homemind-core.052-final" "$binary_path/homemind-selftest.clock"
LD_LIBRARY_PATH="$library_path" "$binary_path/homemind-core.052-final" \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$result_dir/homemind.db" >"$result_dir/core.log" 2>&1 &
core_pid=$!
sleep 2

LD_LIBRARY_PATH="$library_path" "$binary_path/homemind-selftest.clock" \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$result_dir/homemind.db" \
  --node-id CTRL-ESP32-ACT-01 | tee "$result_dir/selftest.log"

echo "CORE_052_DB_CHECK=$(sqlite3 "$result_dir/homemind.db" 'PRAGMA integrity_check;')"
echo "CORE_052_REGRESSION_PASS"
