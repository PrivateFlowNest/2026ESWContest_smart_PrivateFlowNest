#!/bin/sh
# HomeMind 7-inch UI launcher (NODE-RPI5-01, Flutter eLinux bundle, Wayland).
# Installed at /data/share/homemind/ui/homemind-ui-launch.sh next to the bundle
# (homemind_flutter_test, lib/, data/) and started by homemind-ui.service.
#
# Reads KEY=VALUE lines from ui.env without shell evaluation, exports only the known HOMEMIND_* keys
# and starts the UI full screen. The MQTT password is never in ui.env or the environment:
# HOMEMIND_MQTT_PASSWORD_FILE names a 0640 file and the app reads its first line.
#   HOMEMIND_UI_DIR  bundle directory   (default /data/share/homemind/ui)
#   HOMEMIND_UI_ENV  settings file      (default /data/share/homemind/config/ui.env)
# Extra arguments are passed to the bundle (e.g. --rotation=180).
set -eu

ui_dir="${HOMEMIND_UI_DIR:-/data/share/homemind/ui}"
ui_env="${HOMEMIND_UI_ENV:-/data/share/homemind/config/ui.env}"

fail() {
  echo "homemind-ui: $*" >&2
  exit 1
}

[ -r "$ui_env" ] || fail "cannot read $ui_env"
# Never passed on: the app refuses a password value, and so does this launcher.
unset HOMEMIND_MQTT_PASSWORD

cr=$(printf '\r')
while IFS= read -r line || [ -n "$line" ]; do
  line=${line%"$cr"}
  case "$line" in '' | '#'*) continue ;; esac
  key=${line%%=*}
  value=${line#*=}
  [ "$key" != "$line" ] || fail "line without '=' in $ui_env"
  case "$value" in
    \"*\")
      value=${value#\"}
      value=${value%\"}
      ;;
    \'*\')
      value=${value#\'}
      value=${value%\'}
      ;;
  esac
  case "$key" in
    HOMEMIND_PI4_HOST | HOMEMIND_MQTT_TCP_PORT | HOMEMIND_MQTT_USERNAME | \
      HOMEMIND_MQTT_PASSWORD_FILE | HOMEMIND_CLIENT_ID | HOMEMIND_KIOSK | HOMEMIND_EDGE_GUARD | HOMEMIND_DATA_MODE)
      export "$key=$value"
      ;;
    *PASSWORD*)
      fail "$key is not accepted in $ui_env; put the password in the file named by HOMEMIND_MQTT_PASSWORD_FILE"
      ;;
    *) fail "unknown key '$key' in $ui_env" ;;
  esac
done < "$ui_env"

case "${HOMEMIND_PI4_HOST:-}" in
  '' | BROKER_HOST) fail "set HOMEMIND_PI4_HOST in $ui_env (Pi4 broker address)" ;;
esac
if [ -n "${HOMEMIND_MQTT_PASSWORD_FILE:-}" ] && [ ! -r "$HOMEMIND_MQTT_PASSWORD_FILE" ]; then
  fail "cannot read the MQTT password file named in $ui_env (owner/group/mode?)"
fi

# Fail early with a clear message instead of an EGL/Wayland error from the engine.
socket="${WAYLAND_DISPLAY:-wayland-0}"
case "$socket" in /*) ;; *) socket="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$socket" ;; esac
[ -S "$socket" ] || fail "Wayland socket $socket not found (compositor not running, or WAYLAND_DISPLAY/XDG_RUNTIME_DIR wrong)"

exec "$ui_dir/homemind_flutter_test" --bundle="$ui_dir" --fullscreen --width=1024 --height=600 \
  --title=HomeMind --app-id=com.homemind.ui "$@"
