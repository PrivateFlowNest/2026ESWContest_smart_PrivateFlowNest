#!/usr/bin/env bash
# Run first on the laptop that installs the devices (macOS, Linux or Git Bash). Changes nothing.
#   bash deploy/preflight.sh            tools + every payload's release signature and hashes + deploy/site.env
#   bash deploy/preflight.sh --ssh      also log in to NODE-RPI4-01 once with its pinned host key
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/common/payload.sh"
problems=0
ok() { echo "  OK    $*"; }
bad() { echo "  FAIL  $*"; problems=$((problems + 1)); }
note() { echo "  --    $*"; }

echo "== laptop: $(uname -s) $(uname -m), bash ${BASH_VERSION}"
for tool in ssh scp tar git python3; do  # python3: MQTT password hashes (set-mqtt-password.sh)
  if command -v "$tool" >/dev/null 2>&1; then ok "$tool"; else bad "$tool missing"; fi
done
if command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1; then ok "sha256 (sha256sum or shasum)"
else bad "no sha256sum/shasum"; fi
if version="$(hm_openssl version 2>/dev/null)"; then ok "$version"
else bad "OpenSSL 3 missing (macOS: brew install openssl@3)"; fi

echo "== payloads (release signature, then every file hash)"
for node in NODE-RPI4-01 NODE-RPI5-01 NODE-RPI5-02 PHONE-APP; do
  dir="$here/$node/payload"
  if [[ ! -d "$dir" ]]; then note "$node: not in this kit yet (later kit or full payloads.tar)"; continue; fi
  if out="$(payload_verify "$dir" 2>&1)"; then ok "$node ($(ls "$dir" | grep -vc 'SHA256SUMS') files)"
  else bad "$node: $(echo "$out" | tail -1)"; fi
done

echo "== deploy/site.env (addresses only)"
if [[ -f "$here/site.env" ]]; then
  set -a; . "$here/site.env"; set +a
  for name in RPI4_01_HOST RPI5_01_HOST RPI5_02_HOST; do
    value="${!name:-}"
    if [[ -n "$value" ]]; then ok "$name=$value"; else bad "$name empty"; fi
  done
  ok "BROKER_PORT=${BROKER_PORT:-1884}"
else
  bad "no deploy/site.env (copy site.env from the field kit, or site.env.example and fill it)"
fi

echo "== optional tools"
fw="$here/../firmware"
if [[ -x "$fw/.venv-pio/bin/python" || -x "$fw/.venv-pio/Scripts/python" ]]; then ok "PlatformIO venv (ESP flashing)"
else note "no firmware/.venv-pio yet: needed only to flash the ESPs (python3 -m venv firmware/.venv-pio && firmware/.venv-pio/bin/pip install platformio)"; fi
if command -v adb >/dev/null 2>&1; then ok "adb (phone APK)"; else note "no adb: copy the APK to the phone instead (macOS: brew install android-platform-tools)"; fi

if [[ "${1:-}" == "--ssh" ]]; then
  echo "== NODE-RPI4-01 login with the pinned host key (team/homemind-core-ui-2026-10-20/.pi4_known_hosts)"
  host="${RPI4_01_HOST:-10.10.10.2}"
  opts=(-o UserKnownHostsFile="$here/../team/homemind-core-ui-2026-10-20/.pi4_known_hosts" -o StrictHostKeyChecking=yes
        -o ConnectTimeout=8)
  while read -r o; do opts+=("$o"); done < <(hm_ssh_shared_opts)
  if ssh "${opts[@]}" "root@$host" 'echo "  device: $(uname -n), $(date -u +%Y-%m-%dT%H:%M:%SZ) UTC"; opkg list-installed | grep homemind-core'; then
    ok "root@$host (host key matches the pinned key; compare the device time with this laptop: $(date -u +%H:%M:%SZ) UTC)"
  else
    bad "root@$host: cannot log in (cable/address, or the host key differs from the pinned one)"
  fi
fi

echo
if [[ $problems -eq 0 ]]; then echo "PREFLIGHT_PASS"; else echo "PREFLIGHT_FAIL ($problems)"; exit 1; fi
