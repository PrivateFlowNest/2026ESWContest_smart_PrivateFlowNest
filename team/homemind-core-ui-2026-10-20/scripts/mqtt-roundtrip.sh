#!/usr/bin/env bash
set -euo pipefail

broker_host="${1:-10.10.10.2}"
topic="pfn/v1/test/laptop-roundtrip"
payload="laptop_to_pi4_ok"
result_file="$(mktemp)"
subscriber_pid=""

cleanup() {
  if [[ -n "$subscriber_pid" ]] && kill -0 "$subscriber_pid" 2>/dev/null; then
    kill "$subscriber_pid" 2>/dev/null || true
  fi
  rm -f "$result_file"
}
trap cleanup EXIT

mosquitto_sub \
  -h "$broker_host" \
  -p 1883 \
  -i homemind-laptop-check-sub \
  -t "$topic" \
  -q 1 \
  -C 1 \
  -W 10 \
  -v >"$result_file" &
subscriber_pid=$!

sleep 1

mosquitto_pub \
  -h "$broker_host" \
  -p 1883 \
  -i homemind-laptop-check-pub \
  -t "$topic" \
  -q 1 \
  -m "$payload"

wait "$subscriber_pid"
subscriber_pid=""

grep -F "$topic $payload" "$result_file"
echo "MQTT_ROUNDTRIP_PASS broker=$broker_host"
