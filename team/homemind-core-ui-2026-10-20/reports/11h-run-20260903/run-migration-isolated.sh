#!/bin/sh
set -eu

result_dir=/data/share/homemind/testing-11h/migration-regression-01
database="$result_dir/homemind.db"
broker_port=28884
library_path=/data/share/homemind/lib
binary_path=/data/share/homemind/testing-11h/bin/homemind-core.clock

mkdir -p "$result_dir"
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

LD_LIBRARY_PATH="$library_path" "$binary_path" \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$database" >"$result_dir/core.log" 2>&1 &
core_pid=$!
sleep 4
kill -0 "$core_pid"

echo "MIGRATION_DB_CHECK=$(sqlite3 "$database" 'PRAGMA integrity_check;')"
echo "MIGRATION_CORE_PASS"
