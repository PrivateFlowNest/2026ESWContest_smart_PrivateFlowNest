#!/usr/bin/env bash
set -euo pipefail

dependency_root="${1:?dependency root is required}"
project_root="$(cd "$(dirname "$0")/.." && pwd)"
contracts_dir="$(cd "$project_root/../../contracts/pfn.v1" && pwd)"
test_root="$(mktemp -d)"
core_pid=""
broker_pid=""

cleanup() {
  if [[ -n "$core_pid" ]]; then
    kill "$core_pid" 2>/dev/null || true
    wait "$core_pid" 2>/dev/null || true
  fi
  if [[ -n "$broker_pid" ]]; then
    kill "$broker_pid" 2>/dev/null || true
    wait "$broker_pid" 2>/dev/null || true
  fi
  rm -rf -- "$test_root"
}
trap cleanup EXIT

include_dir="$dependency_root/usr/include"
library_dir="$dependency_root/usr/lib/x86_64-linux-gnu"
runtime_path="$library_dir:/usr/lib/x86_64-linux-gnu"

g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 \
  -I"$include_dir" "$project_root/src/core.cpp" \
  -L"$library_dir" -Wl,-rpath,"$library_dir" -lmosquitto -lsqlite3 -pthread \
  -o "$test_root/homemind-core"
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 \
  -I"$include_dir" "$project_root/src/selftest.cpp" \
  -L"$library_dir" -Wl,-rpath,"$library_dir" -lmosquitto -lsqlite3 -pthread \
  -o "$test_root/homemind-selftest"

LD_LIBRARY_PATH="$runtime_path" "$dependency_root/usr/sbin/mosquitto" \
  -p 18883 >"$test_root/mqtt.log" 2>&1 &
broker_pid="$!"
sleep 1

python3 "$project_root/tests/migration-fixture.py" create "$test_root/homemind.db"

LD_LIBRARY_PATH="$runtime_path" "$test_root/homemind-core" \
  --broker-host 127.0.0.1 --broker-port 18883 \
  --database "$test_root/homemind.db" --node-id NODE-RPI4-LOCALTEST \
  --sensor-rules "$contracts_dir/sensor-rules.json" \
  --scenario-rules "$contracts_dir/scenario-rules.json" \
  >"$test_root/core.log" 2>&1 &
core_pid="$!"
sleep 1

LD_LIBRARY_PATH="$runtime_path" "$test_root/homemind-selftest" \
  --broker-host 127.0.0.1 --broker-port 18883 \
  --database "$test_root/homemind.db" --node-id NODE-ESP32-LOCALTEST

python3 "$project_root/tests/migration-fixture.py" verify "$test_root/homemind.db"

echo "LOCAL_CORE_INTEGRATION_PASS"
