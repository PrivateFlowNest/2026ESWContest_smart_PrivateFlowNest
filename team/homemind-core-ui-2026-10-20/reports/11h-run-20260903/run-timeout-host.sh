#!/bin/sh
set -eu

result_dir=/data/share/homemind/testing-11h/clock-timeout-01
broker_port=28884
library_path=/data/share/homemind/lib

mkdir -p "$result_dir"
rm -f "$result_dir/homemind.db" "$result_dir/broker.log" "$result_dir/core.log"
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
  -c /data/share/homemind/testing-11h/mosquitto-timeout.conf \
  >"$result_dir/broker.log" 2>&1 &
broker_pid=$!
sleep 2
LD_LIBRARY_PATH="$library_path" /data/share/homemind/testing-11h/bin/homemind-core.clock \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$result_dir/homemind.db" >"$result_dir/core.log" 2>&1 &
core_pid=$!
sleep 2
echo "TIMEOUT_HOST_READY port=$broker_port"
sleep 60
