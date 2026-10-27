#!/usr/bin/env bash
# Host wrapper (Git Bash on Windows, or Linux). Builds the HomeMind 7-inch UI with open-source
# flutter-elinux in Docker and writes everything to <repo>/native/.deps/ui-elinux/out/<label>
# (gitignored). PC simulation only; nothing is installed on a device.
#
#   tool/elinux/build_elinux.sh images                 # builder + runtime images (amd64, arm64)
#   tool/elinux/build_elinux.sh build <label>          # flutter-elinux build (env below)
#   tool/elinux/build_elinux.sh run <label> <arch>     # weston headless + screenshot
#   tool/elinux/build_elinux.sh run-device <label> <arch>  # same, installed like tool/elinux/device
#
# build env: ELINUX_REPO, ELINUX_REF, PUB_OVERRIDES, ARCHES, DEFINES_x64, DEFINES_arm64, INSTALL_RPATH
#            (defaults below), RUN_TESTS=1 (analyze + app tests on the pinned Flutter SDK)
# run env:   HOMEMIND_PI4_HOST (default host.docker.internal), WAIT_SECONDS (default 25)
set -euo pipefail
export MSYS_NO_PATHCONV=1

here="$(cd "$(dirname "$0")" && pwd)"
app="$(cd "$here/../.." && pwd)"
repo="$(cd "$app/../../.." && pwd)"
out_root="$repo/native/.deps/ui-elinux/out"
winpath() { if command -v cygpath > /dev/null; then cygpath -w "$1"; else echo "$1"; fi; }

# Starting a WSL distro can drop the Docker VM's arm64 binfmt registration ("exec format error").
# Re-register only when an arm64 container no longer starts.
ensure_arm64() {
  docker run --rm --platform linux/arm64 homemind-ui-elinux-runtime:arm64 true > /dev/null 2>&1 \
    || docker run --rm --platform linux/arm64 ubuntu:24.04 true > /dev/null 2>&1 \
    || docker run --privileged --rm tonistiigi/binfmt --install arm64
}

case "${1:-}" in
  images)
    docker buildx build --load --target builder -t homemind-ui-elinux-builder:24.04 "$(winpath "$here")"
    for platform in amd64 arm64; do
      [[ "$platform" == arm64 ]] && ensure_arm64
      docker buildx build --load --platform "linux/$platform" --target runtime \
        -t "homemind-ui-elinux-runtime:$platform" "$(winpath "$here")"
    done
    ;;
  build)
    label="${2:?label}"
    # Defaults = the combination verified on 2026-10-22 (PC simulation): sony/flutter-elinux main
    # (Flutter 3.29.3, Dart 3.7.2) needs mqtt_client <= 10.8.0 (10.8.1+ requires Dart >= 3.8).
    export ELINUX_REPO="${ELINUX_REPO-https://github.com/sony/flutter-elinux.git}"
    export ELINUX_REF="${ELINUX_REF-cdc9622651ee7b217697258cd5ca0b8963f1d6a4}"
    export PUB_OVERRIDES="${PUB_OVERRIDES-mqtt_client:10.8.0}"
    export DEFINES_x64="${DEFINES_x64-HOMEMIND_KIOSK=true HOMEMIND_CLIENT_ID=homemind-ui-7inch}"
    export DEFINES_arm64="${DEFINES_arm64-HOMEMIND_KIOSK=true HOMEMIND_CLIENT_ID=homemind-ui-7inch}"
    # Atlas app folder first (secure-execution start ignores $ORIGIN), then $ORIGIN/lib for the PC
    # simulation and the plain launcher.
    export INSTALL_RPATH="${INSTALL_RPATH-/data/share/usr/atlas/apps/com.atlas.app.homemind_ui/lib:\$ORIGIN/lib}"
    mkdir -p "$out_root/$label"
    docker run --rm \
      -e ELINUX_REPO -e ELINUX_REF -e PUB_OVERRIDES -e ARCHES -e DEFINES_x64 -e DEFINES_arm64 \
      -e RUN_TESTS -e INSTALL_RPATH \
      -v homemind-ui-elinux-cache:/cache \
      -v "$(winpath "$app"):/src/app:ro" \
      -v "$(winpath "$repo/contracts"):/w/contracts:ro" \
      -v "$(winpath "$here/build_in_container.sh"):/build_in_container.sh:ro" \
      -v "$(winpath "$out_root/$label"):/out" \
      homemind-ui-elinux-builder:24.04 bash /build_in_container.sh
    ;;
  run)
    label="${2:?label}"
    arch="${3:?arch x64|arm64}"
    platform=$([[ "$arch" == arm64 ]] && echo arm64 || echo amd64)
    run_dir="$out_root/$label/run-$arch"
    mkdir -p "$run_dir"
    [[ "$arch" == arm64 ]] && ensure_arm64
    docker run --rm --platform "linux/$platform" \
      --add-host host.docker.internal:host-gateway \
      -e HOMEMIND_PI4_HOST="${HOMEMIND_PI4_HOST:-host.docker.internal}" \
      -e WAIT_SECONDS="${WAIT_SECONDS:-25}" \
      -v "$(winpath "$out_root/$label/homemind-ui-elinux-wayland-$arch.tar.gz"):/artifact.tar.gz:ro" \
      -v "$(winpath "$here/run_headless.sh"):/run_headless.sh:ro" \
      -v "$(winpath "$run_dir"):/out" \
      "homemind-ui-elinux-runtime:$platform" bash /run_headless.sh
    ;;
  run-device)
    # Device kit check: install layout + launcher + ui.env (template with the broker filled in) +
    # password file, started as user "network". Used by tool/mqtt_auth_live.sh.
    label="${2:?label}"
    arch="${3:?arch x64|arm64}"
    : "${UI_BROKER_HOST:?}" "${UI_PASSWORD_FILE:?host path of the UI account password file}"
    platform=$([[ "$arch" == arm64 ]] && echo arm64 || echo amd64)
    run_dir="$out_root/$label/${RUN_NAME:-run-device-$arch}"
    mkdir -p "$run_dir"
    sed -e "s/^HOMEMIND_PI4_HOST=.*/HOMEMIND_PI4_HOST=$UI_BROKER_HOST/" \
      -e "s/^HOMEMIND_MQTT_TCP_PORT=.*/HOMEMIND_MQTT_TCP_PORT=${UI_BROKER_PORT:-1884}/" \
      "$here/device/ui.env" > "$run_dir/ui.env"
    [[ "$arch" == arm64 ]] && ensure_arm64
    docker run --rm --platform "linux/$platform" ${DOCKER_NETWORK:+--network "$DOCKER_NETWORK"} \
      -e DEVICE_LAYOUT=1 -e WAIT_SECONDS="${WAIT_SECONDS:-25}" \
      -v "$(winpath "$out_root/$label/homemind-ui-elinux-wayland-$arch.tar.gz"):/artifact.tar.gz:ro" \
      -v "$(winpath "$here/run_headless.sh"):/run_headless.sh:ro" \
      -v "$(winpath "$here/device"):/device:ro" \
      -v "$(winpath "$run_dir/ui.env"):/in/ui.env:ro" \
      -v "$(winpath "$UI_PASSWORD_FILE"):/in/mqtt-password:ro" \
      -v "$(winpath "$run_dir"):/out" \
      "homemind-ui-elinux-runtime:$platform" bash /run_headless.sh
    ;;
  *)
    sed -n '2,14p' "$0"
    exit 2
    ;;
esac
