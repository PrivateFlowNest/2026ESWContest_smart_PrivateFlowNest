#!/bin/sh
set -eu

result_dir=${1:?result directory is required}
duration_seconds=${2:-7200}
interval_ms=${3:-200}

if [ -e "$result_dir" ]; then
  echo "SOAK_SETUP_FAILED result directory already exists: $result_dir" >&2
  exit 2
fi

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

/data/share/homemind/bin/mosquitto \
  -c /data/share/homemind/tests/mosquitto-soak.conf \
  >"$result_dir/broker.log" 2>&1 &
broker_pid=$!

LD_LIBRARY_PATH=/data/share/homemind/lib \
  /data/share/homemind/bin/homemind-core \
  --broker-host 127.0.0.1 \
  --broker-port 28883 \
  --database "$result_dir/homemind-soak.db" \
  --node-id homemind-pi4-soak-core \
  >"$result_dir/core.log" 2>&1 &
core_pid=$!

sleep 3
kill -0 "$broker_pid"
kill -0 "$core_pid"

set +e
LD_LIBRARY_PATH=/data/share/homemind/lib \
  /data/share/homemind/bin/homemind-soaktest \
  --broker-host 127.0.0.1 \
  --broker-port 28883 \
  --database "$result_dir/homemind-soak.db" \
  --node-id homemind-esp32-soak-01 \
  --duration-seconds "$duration_seconds" \
  --interval-ms "$interval_ms" \
  --report-interval-seconds 60 \
  --core-pid "$core_pid" \
  >"$result_dir/soak.log" 2>&1
test_status=$?
set -e

sqlite3 "$result_dir/homemind-soak.db" \
  "select 'events='||count(*) from mqtt_events; select 'telemetry='||count(*) from telemetry; select 'heartbeats='||count(*) from heartbeats; pragma integrity_check;" \
  >"$result_dir/database-summary.txt"

if [ "$test_status" -eq 0 ]; then
  echo PASS >"$result_dir/final-status.txt"
else
  echo FAIL >"$result_dir/final-status.txt"
fi
exit "$test_status"

