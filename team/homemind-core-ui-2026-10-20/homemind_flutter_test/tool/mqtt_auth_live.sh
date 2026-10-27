#!/usr/bin/env bash
# Live MQTT login check for the native UI. PC simulation (Git Bash on Windows, or Linux).
# Starts in Docker, from the demo image homemind-pc-core:0.6.0 (docker compose demo/pc):
#   - a temporary Mosquitto 2: allow_anonymous false, team/.../package/mosquitto.acl
#   - homemind-core logged in as NODE-RPI4-01 (--password-file)
# then runs test/mqtt_auth_live_test.dart (anonymous refused, UI account gets state from core and
# sends pfn/v1/ai/voice/listen, ACL enforced).
# Passwords are random per run, written straight into files (umask 077) under a gitignored temp
# dir, never printed, and deleted on exit.
#
#   ELINUX_LABEL=<label>  also start that eLinux build installed like tool/elinux/device (launcher,
#                         ui.env, password file, user "network") under headless weston, logged in
#                         as NODE-RPI5-01, for each arch in ELINUX_ARCHES (default "x64 arm64").
set -euo pipefail
export MSYS_NO_PATHCONV=1

here="$(cd "$(dirname "$0")" && pwd)"
app="$(cd "$here/.." && pwd)"
team="$(cd "$app/.." && pwd)"
repo="$(cd "$team/../.." && pwd)"
port="${AUTH_BROKER_PORT:-31884}"
image=homemind-pc-core:0.6.0
net=homemind-ui-auth-net
broker=homemind-ui-auth-broker
core=homemind-ui-auth-core
deps="$repo/native/.deps/ui-elinux"
work="$deps/auth-run-$$"
winpath() { if command -v cygpath > /dev/null; then cygpath -w "$1"; else echo "$1"; fi; }

cleanup() {
  mkdir -p "$deps/logs"
  # Mosquitto logs user names, never passwords.
  docker logs "$broker" > "$deps/logs/auth-broker.log" 2>&1 || true
  docker logs "$core" > "$deps/logs/auth-core.log" 2>&1 || true
  docker rm -f "$broker" "$core" > /dev/null 2>&1 || true
  docker network rm "$net" > /dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

users=(NODE-RPI4-01 NODE-RPI5-01 NODE-RPI5-02 homemind-mobile-api CTRL-ESP32-ACT-01 CTRL-ESP8266-LED-01)
umask 077
mkdir -p "$work/secrets" "$work/broker"
: > "$work/broker/passwd.plain"
for user in "${users[@]}"; do
  head -c 18 /dev/urandom | base64 | tr -d '/+=\n' > "$work/secrets/$user"
  printf '%s:%s\n' "$user" "$(cat "$work/secrets/$user")" >> "$work/broker/passwd.plain"
done
cat > "$work/broker/mosquitto.conf" << 'CONF'
per_listener_settings true
listener 1884 0.0.0.0
allow_anonymous false
password_file /etc/hm/passwd
acl_file /etc/hm/mosquitto.acl
persistence false
log_dest stdout
log_type all
CONF

docker rm -f "$broker" "$core" > /dev/null 2>&1 || true
docker network create "$net" > /dev/null 2>&1 || true
docker run -d --name "$broker" --network "$net" -p "127.0.0.1:$port:1884" \
  -v "$(winpath "$work/broker"):/in:ro" \
  -v "$(winpath "$team/package/mosquitto.acl"):/acl/mosquitto.acl:ro" \
  --entrypoint sh "$image" -c 'set -e; mkdir -p /etc/hm
    cp /in/passwd.plain /etc/hm/passwd; cp /in/mosquitto.conf /acl/mosquitto.acl /etc/hm/
    mosquitto_passwd -U /etc/hm/passwd
    chown -R mosquitto:mosquitto /etc/hm; chmod 0700 /etc/hm; chmod 0600 /etc/hm/*
    exec mosquitto -c /etc/hm/mosquitto.conf' > /dev/null
rm -f "$work/broker/passwd.plain"

wait_log() { # wait_log <container> <text>
  for _ in $(seq 60); do
    docker logs "$1" 2>&1 | grep -qF "$2" && return 0
    sleep 0.5
  done
  echo "AUTH_LIVE_FAIL: '$2' not seen in $1" >&2
  return 1
}
wait_log "$broker" "running"

docker run -d --name "$core" --network "$net" \
  -v "$(winpath "$work/secrets/NODE-RPI4-01"):/run/secrets/mqtt-password:ro" \
  "$image" /opt/homemind/bin/homemind-core --broker-host "$broker" --broker-port 1884 \
  --username NODE-RPI4-01 --password-file /run/secrets/mqtt-password \
  --database /tmp/homemind.db --node-id NODE-RPI4-01 \
  --sensor-rules /opt/homemind/contracts/sensor-rules.json \
  --scenario-rules /opt/homemind/contracts/scenario-rules.json \
  --device-registry /opt/homemind/contracts/device-registry.json \
  --task-policy /opt/homemind/contracts/task-policy.json \
  --context-rules /opt/homemind/contracts/context-rules.json > /dev/null
wait_log "$broker" "u'NODE-RPI4-01'"
echo "AUTH_BROKER_UP 127.0.0.1:$port (ACL package/mosquitto.acl @ $(git -C "$(winpath "$repo")" log -1 --format=%h -- team/homemind-core-ui-2026-10-20/package/mosquitto.acl), core as NODE-RPI4-01)"

flutter="${FLUTTER:-}"
[[ -z "$flutter" && -x "$HOME/dev/flutter/bin/flutter" ]] && flutter="$HOME/dev/flutter/bin/flutter"
flutter="${flutter:-flutter}"
(cd "$app" && "$flutter" test test/mqtt_auth_live_test.dart -r expanded \
  --dart-define=RUN_AUTH_BROKER_LIVE=true \
  --dart-define=HOMEMIND_AUTH_BROKER_PORT="$port" \
  --dart-define=HOMEMIND_AUTH_SECRETS_DIR="$(winpath "$work/secrets")") \
  | grep -vE 'available\)|^Resolving|^Downloading|^Got dep|newer versions|pub outdated'
echo "AUTH_LIVE_TEST_PASS"

if [[ -n "${ELINUX_LABEL:-}" ]]; then
  for arch in ${ELINUX_ARCHES:-x64 arm64}; do
    wait_seconds=25
    [[ "$arch" == arm64 ]] && wait_seconds=75
    DOCKER_NETWORK="$net" UI_BROKER_HOST="$broker" UI_BROKER_PORT=1884 \
      UI_PASSWORD_FILE="$work/secrets/NODE-RPI5-01" RUN_NAME="run-auth-$arch" \
      WAIT_SECONDS="$wait_seconds" \
      bash "$here/elinux/build_elinux.sh" run-device "$ELINUX_LABEL" "$arch" > /dev/null
    tail -n 2 "$deps/out/$ELINUX_LABEL/run-auth-$arch/app.log"
  done
  docker logs "$broker" 2>&1 | grep -E "New client connected .*u'NODE-RPI5-01'" | grep -c homemind-ui-7inch |
    sed 's/^/eLinux UI logins as NODE-RPI5-01 with client homemind-ui-7inch: /'
fi
