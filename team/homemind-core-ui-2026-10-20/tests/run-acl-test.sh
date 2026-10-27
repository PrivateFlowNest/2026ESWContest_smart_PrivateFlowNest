#!/usr/bin/env bash
# Checks package/mosquitto.acl against a real Mosquitto (inside tools/docker/dev.Dockerfile).
# Passwords are random per run, exist only in a temp dir and are never printed.
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
port=31884
broker_pid=""
trap '[[ -n "$broker_pid" ]] && kill "$broker_pid" 2>/dev/null; [[ -n "${KEEP:-}" ]] && cat "$work/broker.log"; rm -rf "$work"' EXIT

users=(NODE-RPI4-01 NODE-RPI5-01 NODE-RPI5-02 homemind-mobile-api CTRL-ESP32-ACT-01 CTRL-ESP8266-LED-01)
declare -A secret
umask 077
: >"$work/passwd"
for user in "${users[@]}"; do
  secret[$user]="$(head -c 18 /dev/urandom | base64 | tr -d '/+=')"
  mosquitto_passwd -b "$work/passwd" "$user" "${secret[$user]}"
done
# Mosquitto drops root privileges to the mosquitto user before reading these files.
chmod 0755 "$work"
chown mosquitto:mosquitto "$work/passwd"
chmod 0600 "$work/passwd"
cat >"$work/mosquitto.conf" <<CONF
per_listener_settings true
listener $port 127.0.0.1
allow_anonymous false
password_file $work/passwd
acl_file $project_root/package/mosquitto.acl
persistence false
log_type all
log_dest stdout
CONF
mosquitto -c "$work/mosquitto.conf" >"$work/broker.log" 2>&1 &
broker_pid=$!
sleep 1

# expect_delivery <subscriber> <topic filter> <publisher> <topic> <yes|no> <label>
expect_delivery() {
  local sub_user="$1" filter="$2" pub_user="$3" topic="$4" expected="$5" label="$6"
  local out="$work/out-$RANDOM"
  mosquitto_sub -h 127.0.0.1 -p $port -u "$sub_user" -P "${secret[$sub_user]}" -t "$filter" \
    -C 1 -W 3 >"$out" 2>/dev/null &
  local sub_pid=$!
  sleep 0.5
  mosquitto_pub -h 127.0.0.1 -p $port -u "$pub_user" -P "${secret[$pub_user]}" -t "$topic" \
    -q 1 -m "{\"acl_probe\":\"$label\"}" 2>/dev/null || true
  wait "$sub_pid" 2>/dev/null || true
  local got=no
  grep -q "$label" "$out" && got=yes
  if [[ "$got" != "$expected" ]]; then
    echo "ACL_FAIL $label expected=$expected got=$got" >&2
    exit 1
  fi
  echo "ACL_OK $label delivered=$got"
}

expect_delivery NODE-RPI5-01 'pfn/v1/ai/llm/request' NODE-RPI5-02 pfn/v1/ai/llm/request yes llm_request_to_rpi5_01
expect_delivery NODE-RPI4-01 'pfn/v1/#' NODE-RPI5-02 pfn/v1/ai/llm/request no llm_request_hidden_from_core
expect_delivery NODE-RPI4-01 'pfn/v1/ai/llm/#' NODE-RPI5-01 pfn/v1/ai/llm/result no llm_result_hidden_from_core
expect_delivery NODE-RPI4-01 'pfn/v1/ai/perception' NODE-RPI5-02 pfn/v1/ai/perception yes perception_to_core
expect_delivery NODE-RPI4-01 'pfn/v1/core/task/request' homemind-mobile-api pfn/v1/core/task/request yes phone_task_to_core
expect_delivery CTRL-ESP32-ACT-01 'pfn/v1/device/CTRL-ESP32-ACT-01/command' NODE-RPI4-01 pfn/v1/device/CTRL-ESP32-ACT-01/command yes core_command_to_esp32
expect_delivery CTRL-ESP32-ACT-01 'pfn/v1/device/CTRL-ESP32-ACT-01/command' homemind-mobile-api pfn/v1/device/CTRL-ESP32-ACT-01/command no phone_cannot_command_esp32
expect_delivery CTRL-ESP32-ACT-01 'pfn/v1/device/CTRL-ESP32-ACT-01/command' NODE-RPI5-01 pfn/v1/device/CTRL-ESP32-ACT-01/command no ui_cannot_command_esp32
expect_delivery CTRL-ESP8266-LED-01 'pfn/v1/device/CTRL-ESP8266-LED-01/command' CTRL-ESP32-ACT-01 pfn/v1/device/CTRL-ESP8266-LED-01/command no esp32_cannot_command_esp8266
expect_delivery CTRL-ESP8266-LED-01 'pfn/v1/device/+/command' NODE-RPI4-01 pfn/v1/device/CTRL-ESP32-ACT-01/command no esp8266_cannot_read_esp32_commands
expect_delivery NODE-RPI4-01 'pfn/v1/device/+/ack' CTRL-ESP8266-LED-01 pfn/v1/device/CTRL-ESP8266-LED-01/ack yes esp8266_ack_to_core
expect_delivery NODE-RPI4-01 'pfn/v1/core/task/request' NODE-RPI5-02 pfn/v1/core/task/request yes voice_task_to_core
expect_delivery NODE-RPI5-02 'pfn/v1/ai/voice/listen' NODE-RPI5-01 pfn/v1/ai/voice/listen yes ui_push_to_talk_to_rpi5_02
expect_delivery NODE-RPI4-01 'pfn/v1/#' NODE-RPI5-01 pfn/v1/ai/voice/listen no push_to_talk_hidden_from_core
expect_delivery NODE-RPI5-02 'pfn/v1/ai/voice/listen' homemind-mobile-api pfn/v1/ai/voice/listen no phone_cannot_push_to_talk
# 0.6.2: phone pairing QR on the 7-inch UI. The offer carries a one-time code: only NODE-RPI5-01 may read it.
expect_delivery homemind-mobile-api 'pfn/v1/mobile/pairing/request' NODE-RPI5-01 pfn/v1/mobile/pairing/request yes ui_pairing_request_to_mobile_api
expect_delivery NODE-RPI5-01 'pfn/v1/mobile/pairing/offer' homemind-mobile-api pfn/v1/mobile/pairing/offer yes pairing_offer_to_ui
expect_delivery NODE-RPI4-01 'pfn/v1/#' homemind-mobile-api pfn/v1/mobile/pairing/offer no pairing_offer_hidden_from_core
expect_delivery NODE-RPI5-02 'pfn/v1/mobile/#' homemind-mobile-api pfn/v1/mobile/pairing/offer no pairing_offer_hidden_from_rpi5_02
expect_delivery NODE-RPI5-01 'pfn/v1/mobile/pairing/offer' NODE-RPI5-02 pfn/v1/mobile/pairing/offer no only_mobile_api_writes_offers
expect_delivery homemind-mobile-api 'pfn/v1/mobile/pairing/request' CTRL-ESP32-ACT-01 pfn/v1/mobile/pairing/request no esp32_cannot_ask_for_pairing
# 0.6.5: paired-phone list and unpair on the 7-inch UI. Phone names are personal: only NODE-RPI5-01 reads them.
expect_delivery homemind-mobile-api 'pfn/v1/mobile/phones/request' NODE-RPI5-01 pfn/v1/mobile/phones/request yes ui_phone_list_request_to_mobile_api
expect_delivery NODE-RPI5-01 'pfn/v1/mobile/phones/state' homemind-mobile-api pfn/v1/mobile/phones/state yes phone_list_to_ui
expect_delivery NODE-RPI4-01 'pfn/v1/#' homemind-mobile-api pfn/v1/mobile/phones/state no phone_list_hidden_from_core
expect_delivery NODE-RPI5-02 'pfn/v1/mobile/#' homemind-mobile-api pfn/v1/mobile/phones/state no phone_list_hidden_from_rpi5_02
expect_delivery NODE-RPI5-01 'pfn/v1/mobile/phones/state' NODE-RPI5-02 pfn/v1/mobile/phones/state no only_mobile_api_writes_phone_list
expect_delivery homemind-mobile-api 'pfn/v1/mobile/phones/request' NODE-RPI5-02 pfn/v1/mobile/phones/request no only_ui_unpairs_phones
expect_delivery homemind-mobile-api 'pfn/v1/mobile/phones/request' CTRL-ESP32-ACT-01 pfn/v1/mobile/phones/request no esp32_cannot_unpair_phones
# 0.6.3: phone approvals on their own topic; the phone gateway cannot pose as the 7-inch UI.
expect_delivery NODE-RPI4-01 'pfn/v1/mobile/task/action' homemind-mobile-api pfn/v1/mobile/task/action yes phone_approval_to_core
expect_delivery NODE-RPI4-01 'pfn/v1/ui/#' homemind-mobile-api pfn/v1/ui/action no phone_cannot_pose_as_ui
expect_delivery NODE-RPI4-01 'pfn/v1/mobile/task/action' NODE-RPI5-02 pfn/v1/mobile/task/action no only_mobile_api_writes_phone_actions
expect_delivery NODE-RPI4-01 'pfn/v1/ui/action' NODE-RPI5-01 pfn/v1/ui/action yes ui_approval_to_core

if mosquitto_pub -h 127.0.0.1 -p $port -t pfn/v1/system/heartbeat -m '{}' 2>/dev/null; then
  echo "ACL_FAIL anonymous publish accepted" >&2
  exit 1
fi
echo "ACL_OK anonymous_refused"
echo "ACL_TEST_PASS"
