#!/bin/sh
# HomeMind 7-inch UI on Atlas (NODE-RPI5-01). Started as root by homemind-ui.service.
#
# The Atlas window manager (atlas-shell) gives a screen layer only to apps that the Atlas app manager
# starts: on the real Pi5-01 (2026-10-23) a Wayland client started by systemd got "Failed to find
# layer" even with the registered app id. So the Flutter bundle runs as the Atlas app
# com.atlas.app.homemind_ui (app folder and account made by the Atlas package manager):
#   1. copy the bundle in /data/share/homemind/ui into the app folder (owner = the app's account)
#   2. write the app's settings ($HOME/ui.env of the app) from config/ui.env and copy the MQTT
#      password file next to it (0400, the app's account): that account cannot read
#      /data/share/homemind, and the app manager passes no HOMEMIND_* variables
#   3. restart the app through com.atlas.AppManager1, and start it again whenever it stops (an Atlas edge
#      swipe from the bottom closes the foreground app)
#
#   homemind-ui-atlas.sh supervise   ExecStart: steps 1-3, then check every 2 s
#   homemind-ui-atlas.sh stop        ExecStopPost: stop the app
# The password is never printed. Edit config/ui.env, then: systemctl restart homemind-ui
set -eu

APP_ID=com.atlas.app.homemind_ui
UI_DIR=${HOMEMIND_UI_DIR:-/data/share/homemind/ui}
UI_ENV=${HOMEMIND_UI_ENV:-/data/share/homemind/config/ui.env}
APP_DIR=/data/share/usr/atlas/apps/$APP_ID
APP_HOME=/data/profiles/0/appdata/$APP_ID/home
CHECK_SECONDS=${HOMEMIND_UI_CHECK_SECONDS:-2}

log() { echo "homemind-ui: $*"; }
fail() {
  echo "homemind-ui: $*" >&2
  exit 1
}

am() { busctl call com.atlas.AppManager1 /com/atlas/AppManager1 com.atlas.AppManager1 "$@"; }
app_state() { am GetStatus s "$APP_ID" 2> /dev/null | sed -n 's/.*"state" s "\([^"]*\)".*/\1/p' | tr 'A-Z' 'a-z'; }
start_app() { am Start s "$APP_ID" > /dev/null || log "AppManager Start $APP_ID failed"; }
stop_app() { am Stop s "$APP_ID" > /dev/null 2>&1 || true; }

bundle_sums() {
  (cd "$UI_DIR" && find homemind_flutter_test lib data appinfo.json -type f -exec sha256sum {} \; | sort -k 2)
}

# 1. Bundle into the app folder, only when it differs (sha256 list kept next to it).
sync_bundle() {
  [ -d "$APP_DIR" ] || fail "$APP_DIR missing: $APP_ID is not installed in the Atlas package manager"
  owner=$(stat -c %u:%g "$APP_DIR")
  sums=$(bundle_sums)
  if [ "$sums" = "$(cat "$APP_DIR/.homemind-bundle.sha256" 2> /dev/null || true)" ]; then
    return 0
  fi
  log "copying the bundle into $APP_DIR"
  rm -rf "$APP_DIR/homemind_flutter_test" "$APP_DIR/lib" "$APP_DIR/data" "$APP_DIR/default.profraw"
  cp -a "$UI_DIR/homemind_flutter_test" "$UI_DIR/lib" "$UI_DIR/data" "$UI_DIR/appinfo.json" "$APP_DIR/"
  printf '%s\n' "$sums" > "$APP_DIR/.homemind-bundle.sha256"
  # Same owner and modes as the Atlas package manager gives an app folder.
  chown -R "$owner" "$APP_DIR"
  for entry in homemind_flutter_test lib data appinfo.json .homemind-bundle.sha256; do
    chmod -R 0770 "$APP_DIR/$entry"
  done
}

# 2. Settings and password copy for the app's account.
sync_settings() {
  [ -r "$UI_ENV" ] || fail "cannot read $UI_ENV"
  if grep -qE '^HOMEMIND_PI4_HOST=(BROKER_HOST)?$' "$UI_ENV"; then
    fail "set HOMEMIND_PI4_HOST in $UI_ENV (Pi4 broker address)"
  fi
  [ -d "$APP_HOME" ] || fail "$APP_HOME missing (the Atlas app manager creates it)"
  owner=$(stat -c %u:%g "$APP_HOME")
  password_file=$(sed -n 's/^HOMEMIND_MQTT_PASSWORD_FILE=//p' "$UI_ENV" | tail -n 1 | tr -d "\r\"'")
  umask 077
  {
    echo "# Written by homemind-ui-atlas.sh from $UI_ENV at every start of homemind-ui.service."
    grep -E '^HOMEMIND_(PI4_HOST|MQTT_TCP_PORT|MQTT_USERNAME|CLIENT_ID|KIOSK|EDGE_GUARD|DATA_MODE|CAMERA_PREVIEW_URL)=' "$UI_ENV" | tr -d '\r' || true
    if [ -n "$password_file" ]; then echo "HOMEMIND_MQTT_PASSWORD_FILE=$APP_HOME/mqtt-password"; fi
    # A password value in ui.env is never copied; the UI shows that it was refused.
    if grep -E '^[A-Za-z0-9_]*PASSWORD[A-Za-z0-9_]*=' "$UI_ENV" | grep -qv '^HOMEMIND_MQTT_PASSWORD_FILE='; then
      echo "HOMEMIND_MQTT_PASSWORD="
    fi
  } > "$APP_HOME/.ui.env.new"
  chown "$owner" "$APP_HOME/.ui.env.new"
  mv -f "$APP_HOME/.ui.env.new" "$APP_HOME/ui.env"

  if [ -n "$password_file" ] && [ -r "$password_file" ]; then
    cat "$password_file" > "$APP_HOME/.mqtt-password.new"
    chown "$owner" "$APP_HOME/.mqtt-password.new"
    chmod 0400 "$APP_HOME/.mqtt-password.new"
    mv -f "$APP_HOME/.mqtt-password.new" "$APP_HOME/mqtt-password"
  else
    rm -f "$APP_HOME/mqtt-password"
    log "no MQTT password file (${password_file:-not set in $UI_ENV}): the screen shows the login as waiting"
  fi
}

case "${1:-}" in
  supervise)
    until am ListAllApps > /dev/null 2>&1; do sleep 2; done
    am GetInfo s "$APP_ID" > /dev/null 2>&1 || fail "$APP_ID is not registered in the Atlas app manager"
    stop_app
    sync_bundle
    sync_settings
    start_app
    log "started $APP_ID ($(app_state))"
    while sleep "$CHECK_SECONDS"; do
      state=$(app_state)
      case "$state" in
        launched | running | paused) ;;
        *)
          log "$APP_ID is ${state:-unknown}; starting it again"
          start_app
          ;;
      esac
    done
    ;;
  stop) stop_app ;;
  *)
    sed -n '2,17p' "$0" >&2
    exit 2
    ;;
esac
