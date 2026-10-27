#!/usr/bin/env bash
# Runs inside homemind-ui-elinux-runtime (amd64, or arm64 under QEMU). PC simulation only.
# weston 13 headless (pixman renderer, kiosk shell, one 1024x600 output) + Mesa software EGL,
# unpacks the release tarball mounted at /artifact.tar.gz, starts it, then saves a screenshot
# and logs to /out.
#   env: HOMEMIND_* (read by the app), WAIT_SECONDS, APP_CMD (override, for checks)
#   DEVICE_LAYOUT=1: install like the device kit (tool/elinux/device) and start the launcher as
#     user "network": bundle in /data/share/homemind/ui, /in/ui.env -> config/ui.env,
#     /in/mqtt-password -> secrets/mqtt-password (0640 root:network), launcher from /device.
set -uo pipefail

WAIT_SECONDS="${WAIT_SECONDS:-25}"
export XDG_RUNTIME_DIR=/tmp/xdg-run
mkdir -p -m 0700 "$XDG_RUNTIME_DIR"
cd /out || exit 1
rm -f wayland-screenshot-*.png screenshot-1024x600.png

{
  echo "machine: $(uname -m)"
  weston --version
  dpkg-query -W -f='${Package} ${Version} ${Architecture}\n' weston libwayland-client0 \
    libwayland-egl1 libwayland-cursor0 libxkbcommon0 libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri \
    libfontconfig1
} > runtime-versions.txt 2>&1

sha256sum /artifact.tar.gz | sed 's#/artifact.tar.gz#artifact#' > artifact.sha256
if [[ "${DEVICE_LAYOUT:-0}" == 1 ]]; then
  getent group network > /dev/null || groupadd -r network
  id network > /dev/null 2>&1 || useradd -r -g network -M -s /usr/sbin/nologin network
  base=/data/share/homemind
  install -d -m 0755 "$base/ui" "$base/config" "$base/secrets"
  tar -C "$base/ui" --strip-components=1 -xzf /artifact.tar.gz
  install -m 0755 /device/homemind-ui-launch.sh "$base/ui/homemind-ui-launch.sh"
  install -m 0644 /in/ui.env "$base/config/ui.env"
  install -m 0640 -o root -g network /in/mqtt-password "$base/secrets/mqtt-password"
  bundle="$base/ui"
  # The compositor socket must be reachable for the UI account ([판단] on the device).
  chgrp network "$XDG_RUNTIME_DIR"
  chmod 0750 "$XDG_RUNTIME_DIR"
else
  mkdir -p /opt/homemind-ui
  tar -C /opt/homemind-ui -xzf /artifact.tar.gz
  bundle=/opt/homemind-ui/bundle
fi
{
  echo "## homemind_flutter_test"
  ldd "$bundle/homemind_flutter_test"
  for so in "$bundle"/lib/*.so; do echo "## lib/$(basename "$so")"; ldd "$so"; done
} > ldd.txt 2>&1

weston --backend=headless --renderer=pixman --shell=kiosk --width=1024 --height=600 \
  --socket=wayland-hm --idle-time=0 --debug --log=/out/weston.log &
weston_pid=$!
for _ in $(seq 100); do
  [[ -S "$XDG_RUNTIME_DIR/wayland-hm" ]] && break
  sleep 0.2
done
export WAYLAND_DISPLAY=wayland-hm LIBGL_ALWAYS_SOFTWARE=1

if [[ "${DEVICE_LAYOUT:-0}" == 1 ]]; then
  chgrp network "$XDG_RUNTIME_DIR/wayland-hm"
  chmod 0770 "$XDG_RUNTIME_DIR/wayland-hm"
  default_cmd="setpriv --reuid=network --regid=network --init-groups \
    env XDG_CACHE_HOME=/tmp $bundle/homemind-ui-launch.sh"
else
  default_cmd="$bundle/homemind_flutter_test --bundle=$bundle --fullscreen \
    --width=1024 --height=600 --title=HomeMind --app-id=com.homemind.ui"
fi
app_cmd="${APP_CMD:-$default_cmd}"
echo "app: $app_cmd  (HOMEMIND_PI4_HOST=${HOMEMIND_PI4_HOST:-})" > app.log
start=$(date +%s)
$app_cmd >> app.log 2>&1 &
app_pid=$!
sleep "$WAIT_SECONDS"

if kill -0 "$app_pid" 2> /dev/null; then
  echo "app alive after ${WAIT_SECONDS}s" | tee -a app.log
else
  wait "$app_pid"
  echo "APP_EXITED status=$? after $(($(date +%s) - start))s" | tee -a app.log
fi
weston-screenshooter >> app.log 2>&1
shot=$(ls -t wayland-screenshot-*.png 2> /dev/null | head -n 1)
if [[ -n "$shot" ]]; then
  mv "$shot" screenshot-1024x600.png
  file screenshot-1024x600.png | tee -a app.log
else
  echo "NO_SCREENSHOT" | tee -a app.log
fi
kill "$app_pid" "$weston_pid" 2> /dev/null
wait 2> /dev/null
exit 0
