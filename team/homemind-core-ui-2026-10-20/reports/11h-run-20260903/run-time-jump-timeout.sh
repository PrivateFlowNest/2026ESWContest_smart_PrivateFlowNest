#!/bin/sh
set -eu

result_dir=/data/share/homemind/testing-11h/clock-jump-timeout-01
database="$result_dir/homemind.db"
broker_port=28884
library_path=/data/share/homemind/lib

mkdir -p "$result_dir"
rm -f "$database" "$result_dir/broker.log" "$result_dir/core.log" \
  "$result_dir/modifier.log"
broker_pid=""
core_pid=""
modifier_pid=""
cleanup() {
  if [ -n "$modifier_pid" ]; then
    kill "$modifier_pid" 2>/dev/null || true
    wait "$modifier_pid" 2>/dev/null || true
  fi
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
  -c /data/share/homemind/testing-11h/mosquitto-timeout.conf \
  >"$result_dir/broker.log" 2>&1 &
broker_pid=$!
sleep 2
LD_LIBRARY_PATH="$library_path" \
  /data/share/homemind/testing-11h/bin/homemind-core.052-final \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$database" >"$result_dir/core.log" 2>&1 &
core_pid=$!
sleep 2
kill -0 "$broker_pid"
kill -0 "$core_pid"

(
  tries=0
  while [ "$tries" -lt 1000 ]; do
    pending=$(sqlite3 "$database" "select count(*) from commands where status='pending';")
    if [ "$pending" -gt 0 ]; then
      if sqlite3 -cmd '.timeout 5000' "$database" \
          "update commands set updated_at_ms=4102444800000, expires_at_ms=4102444800000 where status='pending';"; then
        echo "FUTURE_WALL_TIME_INJECTED pending=$pending"
        exit 0
      fi
      echo "FUTURE_WALL_TIME_INJECTION_RETRY database_locked" >&2
    fi
    tries=$((tries + 1))
    sleep 0.1
  done
  echo "FUTURE_WALL_TIME_INJECTION_FAILED"
  exit 1
) >"$result_dir/modifier.log" 2>&1 &
modifier_pid=$!

echo "TIME_JUMP_TIMEOUT_HOST_READY port=$broker_port"
wait "$modifier_pid"
modifier_pid=""
sleep 30
