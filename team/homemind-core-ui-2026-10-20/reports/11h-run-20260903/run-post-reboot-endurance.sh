#!/bin/sh
set -eu

result_dir=/data/share/homemind/testing-11h/post-reboot-endurance-20260903-01
duration_seconds=25200
interval_ms=1000
broker_port=28886
library_path=/data/share/homemind/lib
production_database=/data/share/homemind/db/homemind.db
expected_core_sha256=877f37df26bebe442d20552739426f7c044533dd27d356718b3b3978fa704e81

if [ -e "$result_dir" ]; then
  echo "ENDURANCE_SETUP_FAILED result directory already exists: $result_dir" >&2
  exit 2
fi

mkdir -p "$result_dir"
test "$(sha256sum /data/share/homemind/bin/homemind-core | cut -d' ' -f1)" = \
  "$expected_core_sha256"
broker_pid=""
core_pid=""
monitor_pid=""
cleanup() {
  if [ -n "$monitor_pid" ]; then
    kill "$monitor_pid" 2>/dev/null || true
    wait "$monitor_pid" 2>/dev/null || true
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
  -p "$broker_port" >"$result_dir/broker.log" 2>&1 &
broker_pid=$!

LD_LIBRARY_PATH="$library_path" /data/share/homemind/bin/homemind-core \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$result_dir/homemind-endurance.db" \
  --node-id NODE-RPI4-ENDURANCE \
  >"$result_dir/core.log" 2>&1 &
core_pid=$!

sleep 3
kill -0 "$broker_pid"
kill -0 "$core_pid"

(
  while kill -0 "$core_pid" 2>/dev/null; do
    uptime_seconds=$(cut -d. -f1 /proc/uptime)
    core_rss_kib=$(awk '/VmRSS:/ {print $2}' "/proc/$core_pid/status")
    mqtt_state=$(systemctl is-active homemind-mqtt.service)
    production_core_state=$(systemctl is-active homemind-core.service)
    echo "ENDURANCE_MONITOR uptime_seconds=$uptime_seconds test_core_rss_kib=$core_rss_kib mqtt=$mqtt_state production_core=$production_core_state"
    sleep 300
  done
) >"$result_dir/monitor.log" 2>&1 &
monitor_pid=$!

set +e
LD_LIBRARY_PATH="$library_path" /data/share/homemind/bin/homemind-soaktest \
  --broker-host 127.0.0.1 \
  --broker-port "$broker_port" \
  --database "$result_dir/homemind-endurance.db" \
  --node-id CTRL-ESP32-ACT-01 \
  --duration-seconds "$duration_seconds" \
  --interval-ms "$interval_ms" \
  --report-interval-seconds 300 \
  --core-pid "$core_pid" \
  >"$result_dir/soak.log" 2>&1
test_status=$?
set -e

test_integrity=$(sqlite3 "$result_dir/homemind-endurance.db" 'PRAGMA integrity_check;')
production_integrity=$(sqlite3 "$production_database" 'PRAGMA integrity_check;')
automatic_delete=$(sqlite3 "$production_database" "select value from metadata where key='automatic_delete_enabled';")
production_mqtt=$(systemctl is-active homemind-mqtt.service)
production_core=$(systemctl is-active homemind-core.service)
production_core_sha256=$(sha256sum /data/share/homemind/bin/homemind-core | cut -d' ' -f1)

{
  echo "test_integrity=$test_integrity"
  echo "production_integrity=$production_integrity"
  echo "automatic_delete=$automatic_delete"
  echo "production_mqtt=$production_mqtt"
  echo "production_core=$production_core"
  echo "production_core_sha256=$production_core_sha256"
} >"$result_dir/final-system-status.txt"

if [ "$test_status" -eq 0 ] && [ "$test_integrity" = ok ] && \
   [ "$production_integrity" = ok ] && [ "$automatic_delete" = false ] && \
   [ "$production_mqtt" = active ] && [ "$production_core" = active ] && \
   [ "$production_core_sha256" = "$expected_core_sha256" ]; then
  echo PASS >"$result_dir/final-status.txt"
  exit 0
fi

echo FAIL >"$result_dir/final-status.txt"
exit 1
