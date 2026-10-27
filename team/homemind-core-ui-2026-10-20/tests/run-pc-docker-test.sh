#!/usr/bin/env bash
# PC (x86_64) integration test inside tools/docker/dev.Dockerfile. Simulation only: no Pi, no ESP.
#
#   docker build -t homemind-dev:24.04 -f tools/docker/dev.Dockerfile tools/docker
#   docker run --rm -v <repo>:/repo:ro homemind-dev:24.04 \
#     bash /repo/team/homemind-core-ui-2026-10-20/tests/run-pc-docker-test.sh [soak_seconds]
set -euo pipefail

soak_seconds="${1:-120}"
project_root="$(cd "$(dirname "$0")/.." && pwd)"
contracts_dir="$(cd "$project_root/../../contracts/pfn.v1" && pwd)"
work="$(mktemp -d)"
broker_pid=""
core_pid=""

cleanup() {
  for pid in "$core_pid" "$broker_pid"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

cmake -S "$project_root" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/build" >/dev/null
echo "PC_BUILD_PASS"

start_core() {
  local database="$1" port="$2" node_id="$3" policy="${4:-$contracts_dir/task-policy.json}"
  "$work/build/homemind-core" \
    --broker-host 127.0.0.1 --broker-port "$port" --database "$database" --node-id "$node_id" \
    --sensor-rules "$contracts_dir/sensor-rules.json" \
    --scenario-rules "$contracts_dir/scenario-rules.json" \
    --device-registry "$contracts_dir/device-registry.json" \
    --task-policy "$policy" \
    --context-rules "$contracts_dir/context-rules.json" \
    >"$work/core-$port.log" 2>&1 &
  core_pid="$!"
}

# 1. Startup refuses a scenario that targets a device that is not wired.
broken="$work/broken-scenarios.json"
python3 - "$contracts_dir/scenario-rules.json" "$broken" <<'PY'
import json, sys
rules = json.load(open(sys.argv[1], encoding="utf-8"))
rules["scenarios"][2]["commands"][0]["target_device_id"] = "LED-GUIDE-01"
json.dump(rules, open(sys.argv[2], "w", encoding="utf-8"))
PY
if "$work/build/homemind-core" --broker-host 127.0.0.1 --broker-port 1 \
    --database "$work/refuse.db" --node-id NODE-RPI4-LOCALTEST \
    --sensor-rules "$contracts_dir/sensor-rules.json" --scenario-rules "$broken" \
    --device-registry "$contracts_dir/device-registry.json" \
    --task-policy "$contracts_dir/task-policy.json" \
    --context-rules "$contracts_dir/context-rules.json" >"$work/refuse.log" 2>&1; then
  echo "core accepted a scenario with a not_wired device" >&2
  exit 1
fi
grep -q "DEVICE_NOT_WIRED" "$work/refuse.log"
echo "STARTUP_REGISTRY_CHECK_PASS"

# 2. --password is refused; only --password-file is accepted.
if "$work/build/homemind-core" --username core --password x >"$work/password.log" 2>&1; then
  echo "core accepted --password" >&2
  exit 1
fi
grep -q "password-file" "$work/password.log"
echo "PASSWORD_ARGUMENT_REFUSED_PASS"

# 3. Selftest against a migrated legacy database.
mosquitto -p 18883 >"$work/broker-selftest.log" 2>&1 &
broker_pid="$!"
sleep 1
python3 "$project_root/tests/migration-fixture.py" create "$work/homemind.db"
start_core "$work/homemind.db" 18883 NODE-RPI4-LOCALTEST
sleep 1
"$work/build/homemind-selftest" --broker-host 127.0.0.1 --broker-port 18883 \
  --database "$work/homemind.db" --node-id NODE-ESP32-LOCALTEST | tee "$work/selftest.log"
python3 "$project_root/tests/migration-fixture.py" verify "$work/homemind.db"
kill "$core_pid"; wait "$core_pid" 2>/dev/null || true; core_pid=""
kill "$broker_pid"; wait "$broker_pid" 2>/dev/null || true; broker_pid=""

# 4. Proposal expiry (task-policy.json policy_version 3) with a short policy copy: default 45 s, entrance door 30 s.
short_policy="$work/task-policy-short.json"
python3 - "$contracts_dir/task-policy.json" "$short_policy" <<'PY'
import json, sys
policy = json.load(open(sys.argv[1], encoding="utf-8"))
policy["proposal_ttl_seconds"] = 45
for rule in policy["rules"]:
    if "proposal_ttl_seconds" in rule:
        rule["proposal_ttl_seconds"] = 30
json.dump(policy, open(sys.argv[2], "w", encoding="utf-8"))
PY
mosquitto -p 38883 >"$work/broker-expiry.log" 2>&1 &
broker_pid="$!"
sleep 1
start_core "$work/expiry.db" 38883 NODE-RPI4-LOCALTEST "$short_policy"
mosquitto_sub -h 127.0.0.1 -p 38883 -t pfn/v1/core/task/update >"$work/expiry-updates.jsonl" 2>/dev/null &
sub_pid="$!"
sleep 1
python3 "$project_root/tests/proposal-expiry-check.py" "$contracts_dir" "$work/expiry.db" 38883
kill "$sub_pid" 2>/dev/null || true
[[ "$(grep -c PROPOSAL_EXPIRED "$work/core-38883.log")" -eq 2 ]]
grep -q '"code":"PROPOSAL_EXPIRED"' "$work/expiry-updates.jsonl"
echo "PROPOSAL_EXPIRY_PASS"
kill "$core_pid"; wait "$core_pid" 2>/dev/null || true; core_pid=""
kill "$broker_pid"; wait "$broker_pid" 2>/dev/null || true; broker_pid=""
# 5. Short soak (the Pi4 run used 7200 s; this is the PC shortened version).
mosquitto -p 28883 >"$work/broker-soak.log" 2>&1 &
broker_pid="$!"
sleep 1
start_core "$work/soak.db" 28883 homemind-pi4-soak-core
sleep 2
"$work/build/homemind-soaktest" --broker-host 127.0.0.1 --broker-port 28883 \
  --database "$work/soak.db" --node-id homemind-esp32-soak-01 \
  --duration-seconds "$soak_seconds" --interval-ms 200 --report-interval-seconds 30 \
  --core-pid "$core_pid" | tee "$work/soak.log"
grep -q '"passed":true' "$work/soak.log"

echo "PC_DOCKER_TEST_PASS real_hardware_executed=false soak_seconds=$soak_seconds"
