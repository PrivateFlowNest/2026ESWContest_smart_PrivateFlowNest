#!/bin/sh
set -eu

result_dir=${1:?result directory is required}

if [ -e "$result_dir" ]; then
  echo "STORAGE_TEST_SETUP_FAILED result directory already exists: $result_dir" >&2
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
  --database "$result_dir/storage-warning.db" \
  --node-id homemind-pi4-storage-test-core \
  --storage-warning-percent 1 \
  >"$result_dir/core.log" 2>&1 &
core_pid=$!

sleep 5
kill -0 "$broker_pid"
kill -0 "$core_pid"

grep "STORAGE_WARNING" "$result_dir/core.log" >"$result_dir/warning-evidence.txt"
sqlite3 "$result_dir/storage-warning.db" \
  "select printf('used=%.2f%% threshold=%d%% warning=%d',filesystem_used_percent,warning_threshold_percent,warning_active) from storage_status where singleton_id=1; pragma integrity_check;" \
  >"$result_dir/database-summary.txt"
echo PASS >"$result_dir/final-status.txt"

