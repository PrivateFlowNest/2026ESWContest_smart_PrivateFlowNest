#!/bin/sh
# Checks homemind-ui-launch.sh with a fake bundle (prints its environment and arguments).
# Linux only (needs python3 for a Wayland-like UNIX socket). PC simulation, no device:
#   docker run --rm -v "<this dir>:/device:ro" homemind-ui-elinux-builder:24.04 sh /device/test_launcher.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/ui" "$work/run"
cat > "$work/ui/homemind_flutter_test" << 'FAKE'
#!/bin/sh
for key in HOMEMIND_PI4_HOST HOMEMIND_MQTT_TCP_PORT HOMEMIND_MQTT_USERNAME \
  HOMEMIND_MQTT_PASSWORD_FILE HOMEMIND_CLIENT_ID HOMEMIND_KIOSK; do
  eval "echo $key=\${$key:-}"
done
[ -z "${HOMEMIND_MQTT_PASSWORD+x}" ] && echo "password-env=absent" || echo "password-env=PRESENT"
echo "args=$*"
FAKE
chmod +x "$work/ui/homemind_flutter_test"
python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$work/run/wayland-0"
umask 077
head -c 18 /dev/urandom | base64 > "$work/mqtt-password"

check() { # check <name> <expect ok|fail> <grep pattern> -- launcher runs with ui.env from stdin
  name=$1 expect=$2 pattern=$3
  cat > "$work/ui.env"
  if HOMEMIND_UI_DIR="$work/ui" HOMEMIND_UI_ENV="$work/ui.env" XDG_RUNTIME_DIR="$work/run" \
    WAYLAND_DISPLAY=wayland-0 HOMEMIND_MQTT_PASSWORD=from-caller-env \
    sh "$here/homemind-ui-launch.sh" --rotation=0 > "$work/out" 2>&1; then got=ok; else got=fail; fi
  if [ "$got" != "$expect" ] || ! grep -q -- "$pattern" "$work/out"; then
    echo "LAUNCHER_FAIL $name (expected $expect, got $got)"
    sed 's/^/  | /' "$work/out"
    exit 1
  fi
  if grep -qF "$(cat "$work/mqtt-password")" "$work/out"; then
    echo "LAUNCHER_FAIL $name printed the password"
    exit 1
  fi
  echo "LAUNCHER_OK $name"
}

sed -e 's/^HOMEMIND_PI4_HOST=.*/HOMEMIND_PI4_HOST=10.10.10.2/' \
  -e "s#^HOMEMIND_MQTT_PASSWORD_FILE=.*#HOMEMIND_MQTT_PASSWORD_FILE=$work/mqtt-password#" \
  "$here/ui.env" > "$work/template.env"

check template ok 'HOMEMIND_MQTT_USERNAME=NODE-RPI5-01' < "$work/template.env"
grep -q 'password-env=absent' "$work/out" || { echo "LAUNCHER_FAIL caller password env leaked"; exit 1; }
grep -q 'HOMEMIND_MQTT_TCP_PORT=1884' "$work/out"
grep -q 'HOMEMIND_CLIENT_ID=homemind-ui-7inch' "$work/out"
grep -q -- "args=--bundle=$work/ui --fullscreen --width=1024 --height=600 --title=HomeMind --app-id=com.homemind.ui --rotation=0" "$work/out"

printf 'HOMEMIND_PI4_HOST="10.0.0.5"\r\nHOMEMIND_KIOSK=true\r\n' |
  check crlf_and_quotes ok 'HOMEMIND_PI4_HOST=10.0.0.5$'
check placeholder fail 'set HOMEMIND_PI4_HOST' < "$here/ui.env"
{ cat "$work/template.env"; echo "HOMEMIND_MQTT_PASSWORD=$(cat "$work/mqtt-password")"; } |
  check password_value fail 'HOMEMIND_MQTT_PASSWORD is not accepted'
{ cat "$work/template.env"; echo "MQTT_PASSWORD=x"; } | check other_password_key fail 'not accepted'
{ cat "$work/template.env"; echo "HOMEMIND_TYPO=1"; } | check unknown_key fail "unknown key 'HOMEMIND_TYPO'"
{ cat "$work/template.env"; echo "no equals sign"; } | check malformed fail "line without '='"
sed "s#^HOMEMIND_MQTT_PASSWORD_FILE=.*#HOMEMIND_MQTT_PASSWORD_FILE=$work/missing#" "$work/template.env" |
  check missing_password_file fail 'cannot read the MQTT password file'
rm "$work/run/wayland-0"
check no_wayland_socket fail 'Wayland socket' < "$work/template.env"
echo "LAUNCHER_TEST_PASS"
