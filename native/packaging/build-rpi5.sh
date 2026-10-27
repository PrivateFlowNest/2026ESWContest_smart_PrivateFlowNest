#!/usr/bin/env bash
# Build the RPI5 native services for aarch64 and pack one IPK per node (PC, QEMU arm64 emulation).
# Runs inside the arm64 dev image (Ubuntu 24.04 = glibc 2.39 like LG AI Native OS scarthgap):
#   docker run --rm --platform linux/arm64 -v "<repo>:/w" -v hm-native-build-arm64:/build \
#     homemind-native-dev:24.04-arm64 bash /w/native/packaging/build-rpi5.sh [--skip-tests]
# Output: /w/native/dist-ipk/homemind-rpi5-{llm,perception}_<version>_aarch64.ipk + SHA256SUMS
# Models above a few MB (EXAONE GGUF, Whisper) are NOT packed; deploy/NODE-RPI5-0X/install.sh copies them.
set -euo pipefail

W=/w
NATIVE=$W/native
PKG=$NATIVE/packaging
OUT=$NATIVE/dist-ipk
TOOLS=$NATIVE/.deps/arm64-tools/bin
MODELS=$NATIVE/.deps/models
skip_tests=0
nodes="01 02 ui rpi4api"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-tests) skip_tests=1; shift;;
    --node) nodes="$2"; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done

[[ "$(uname -m)" == "aarch64" ]] || { echo "run in the arm64 image" >&2; exit 1; }
version="$(sed -n 's/^project(homemind_native VERSION \([0-9.]*\).*/\1/p' "$NATIVE/CMakeLists.txt")"

echo "== build $version (Release, aarch64)"
cmake -S "$NATIVE" -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHM_BUILD_TESTS=ON \
  -DCMAKE_C_FLAGS="-mcpu=cortex-a76" -DCMAKE_CXX_FLAGS="-mcpu=cortex-a76" >/dev/null
# Build only what the selected nodes ship (+ their unit tests), so unrelated work in progress cannot block a package.
targets_01="homemind-llm homemind-clock-sync homemind_clock_sync_tests hm_common_tests hm_text_tests hm_guard_tests hm_parser_tests homemind_llm_tests"
tests_01="homemind_clock_sync|hm_common|hm_text|hm_guard|hm_parser|homemind_llm"
targets_02="homemind-perception homemind-voice-text homemind-vision homemind-clock-sync homemind_clock_sync_tests hm_vision_tests hm_partial_tests hm_vosk_tests hm_common_tests hm_text_tests hm_parser_tests hm_audio_tests hm_stt_tests hm_context_tests homemind_perception_tests homemind_voice_text_tests"
tests_02="homemind_clock_sync|hm_common|hm_text|hm_parser|hm_audio|hm_stt|hm_context|homemind_perception|homemind_voice_text|hm_vision|hm_partial|hm_vosk"
targets_ui="hm_common_tests"
targets_rpi4api="hm_common_tests"
tests_rpi4api="hm_common"
tests_ui="hm_common"
targets=""; tests=""
for node in $nodes; do
  eval "targets=\"\$targets \$targets_$node\""
  eval "tests=\"\${tests:+\$tests|}\$tests_$node\""
done
if [[ -n "${HM_EXTRA_TARGETS:-}" ]]; then targets="$targets $HM_EXTRA_TARGETS"; fi
cmake --build /build --target $targets
if [[ $skip_tests -eq 0 ]]; then
  echo "== ctest (QEMU arm64 emulation, not a Pi)"
  (cd /build && ctest --output-on-failure -R "^($tests)_tests\$")
fi

# Libraries the device provides (glibc family, libstdc++, libgcc_s, OpenSSL 3): same assumption that
# homemind-core 0.6.0 relies on. Everything else is bundled into /data/share/homemind/lib.
# libsystemd (sd-bus for Camera1) must be the device's own, matching its systemd.
SYSTEM_LIBS='^(linux-vdso|ld-linux-aarch64|libc|libm|libdl|librt|libpthread|libstdc\+\+|libgcc_s|libssl|libcrypto|libsystemd|libcap|libgcrypt|libgpg-error|liblzma|libzstd|liblz4)\.so'

bundle_libs() {  # bundle_libs <dest_lib_dir> <elf>...
  local dest=$1; shift
  local lib name path
  for elf in "$@"; do
    while read -r name path; do
      [[ -n "$path" && -f "$path" ]] || continue
      [[ "$name" =~ $SYSTEM_LIBS ]] && continue
      cp -L "$path" "$dest/$name"
    done < <(ldd "$elf" | awk '/=>/ {print $1, $3}')
  done
}

make_ipk() {  # make_ipk <package> <node-dir> <units...>
  local package=$1 node=$2; shift 2
  local units="$*"
  # A node directory may pin its own package version (VERSION file); otherwise the native project's.
  local version="$version"
  if [[ -f "$PKG/$node/VERSION" ]]; then version="$(tr -d ' \r\n' < "$PKG/$node/VERSION")"; fi
  local root=/tmp/pkg-$package
  local data=$root/data
  local hm=$data/data/share/homemind
  local unit_dir=$data/data/share/usr/lib/systemd/system
  rm -rf "$root"
  mkdir -p "$hm"/{bin,lib,share,package-defaults} "$unit_dir" "$root/control" "$OUT"
  cp -a "/tmp/stage-$package/bin/." "$hm/bin/"
  cp -a "/tmp/stage-$package/share/." "$hm/share/"
  if [[ -d "/tmp/stage-$package/lib" ]]; then cp -a "/tmp/stage-$package/lib/." "$hm/lib/"; fi
  if [[ -d "/tmp/stage-$package/ui" ]]; then mkdir -p "$hm/ui" && cp -a "/tmp/stage-$package/ui/." "$hm/ui/"; fi
  if [[ "${HM_BUNDLE_LIBS:-1}" == 1 ]] && compgen -G "$hm/bin/*" >/dev/null; then
    LD_LIBRARY_PATH=/opt/onnxruntime/lib bundle_libs "$hm/lib" "$hm"/bin/*
  fi
  for unit in $units; do  # node units, else the shared ones (homemind-clock-sync)
    if [[ -f "$PKG/$node/units/$unit" ]]; then cp "$PKG/$node/units/$unit" "$unit_dir/"; else cp "$PKG/common/units/$unit" "$unit_dir/"; fi
  done
  cp "$PKG/$node/config/"* "$hm/package-defaults/"
  local service_id
  service_id="$(sed -n 's/.*"id": "\([^"]*\)".*/\1/p' "$PKG/$node/serviceinfo.json")"
  local serviceinfo_dir="$data/data/share/usr/atlas/services/$service_id"
  mkdir -p "$serviceinfo_dir"
  sed "s/@VERSION@/$version/" "$PKG/$node/serviceinfo.json" > "$serviceinfo_dir/serviceinfo.json"
  sed -e "s/@VERSION@/$version/" "$PKG/$node/control" > "$root/control/control"
  for script in postinst prerm; do
    sed -e "s/@PACKAGE@/$package/" -e "s/@UNITS@/$units/" "$PKG/common/$script.in" > "$root/control/$script"
    extra="$PKG/$node/$script-extra.sh"
    python3 - "$root/control/$script" "$extra" <<'PY'
import pathlib, sys
script = pathlib.Path(sys.argv[1]); extra = pathlib.Path(sys.argv[2])
script.write_text(script.read_text().replace("@POSTINST_EXTRA@", extra.read_text() if extra.is_file() else ""))
PY
    chmod 0755 "$root/control/$script"
  done
  # Explicit modes: a Windows checkout gives every file 0777.
  find "$data" -type d -exec chmod 0755 {} +
  find "$hm/share" "$hm/package-defaults" "$unit_dir" "$data/data/share/usr/atlas" -type f -exec chmod 0644 {} +
  find "$hm/bin" -type f -exec chmod 0755 {} +
  find "$hm/lib" -type f -exec chmod 0644 {} +
  printf '2.0\n' > "$root/debian-binary"
  tar --numeric-owner --owner=0 --group=0 -C "$root/control" -czf "$root/control.tar.gz" .
  tar --numeric-owner --owner=0 --group=0 -C "$data" -czf "$root/data.tar.gz" .
  local ipk="$OUT/${package}_${version}_aarch64.ipk"
  rm -f "$ipk"
  (cd "$root" && ar r "$ipk" debian-binary control.tar.gz data.tar.gz 2>/dev/null)
  echo "-- $ipk $(stat -c %s "$ipk") bytes"
  tar -tzf "$root/data.tar.gz" | sed 's/^/   /'
}

stage() {  # stage <package> <file-in-bin...>
  local package=$1; shift
  local dir=/tmp/stage-$package
  rm -rf "$dir"; mkdir -p "$dir/bin" "$dir/share"
  for file in "$@"; do
    [[ -f "$file" ]] || { echo "missing $file" >&2; exit 1; }
    cp "$file" "$dir/bin/"
  done
  aarch64-linux-gnu-strip "$dir"/bin/* 2>/dev/null || strip "$dir"/bin/*
}

share() {  # share <package> <source> <dest-relative-to-share>
  local dir=/tmp/stage-$1/share
  mkdir -p "$dir/$(dirname "$3")"
  cp -a "$2" "$dir/$3"
}

find_bin() { find /build -type f -name "$1" -perm -u+x | head -1; }

if [[ " $nodes " == *" 01 "* ]]; then
echo "== NODE-RPI5-01 (LLM)"
stage homemind-rpi5-llm "$(find_bin homemind-llm)" "$(find_bin homemind-clock-sync)" "$TOOLS/llama-server"
share homemind-rpi5-llm "$W/contracts/pfn.v1" contracts/pfn.v1
share homemind-rpi5-llm "$W/contracts/rpi5-internal" contracts/rpi5-internal
share homemind-rpi5-llm "$W/services/exaone-edge/config/device-capabilities.json" device-capabilities.json
share homemind-rpi5-llm "$PKG/rpi5-01/cpu-limit.sh" cpu-limit.sh
make_ipk homemind-rpi5-llm rpi5-01 homemind-cpu-limit.service homemind-exaone.service homemind-llm.service homemind-clock-sync.service homemind-clock-sync.timer
fi

if [[ " $nodes " == *" 02 "* ]]; then
echo "== NODE-RPI5-02 (perception)"
stage homemind-rpi5-perception "$(find_bin homemind-perception)" "$(find_bin homemind-voice-text)" "$(find_bin homemind-vision)" "$(find_bin homemind-clock-sync)" "$TOOLS/whisper-server"   "$TOOLS/whisper-cli"
share homemind-rpi5-perception "$W/contracts/pfn.v1" contracts/pfn.v1
share homemind-rpi5-perception "$W/contracts/rpi5-internal" contracts/rpi5-internal
share homemind-rpi5-perception "$W/services/exaone-edge/config/device-capabilities.json" device-capabilities.json
share homemind-rpi5-perception "$W/services/edge-ai/context-fusion/config/audio-policy-v6.3-hold15.json" \
  audio-policy-v6.3-hold15.json
# v6.2.1 (release 2) ships as the rollback policy: point fusion_policy in rpi5-02.json at it to go back.
share homemind-rpi5-perception "$W/services/edge-ai/context-fusion/config/audio-policy-v6-ensemble-50-50.json" \
  audio-policy-v6-ensemble-50-50.json
share homemind-rpi5-perception "$MODELS/yamnet" models/yamnet
share homemind-rpi5-perception "$MODELS/activity-head" models/activity-head
# Vision stays off until a person creates config/rpi5-02-vision.json: the default example uses the "background"
# detector (one-axis doll, no model) with zones.rail; the yolox example needs a trained FIGURE model.
for example in vision.device.example.json:rpi5-02-vision.json.example \
               vision.device.yolox.example.json:rpi5-02-vision.yolox.json.example \
               zones.rail.example.json:zones.rail.json.example zones.indoor.example.json:zones.indoor.json.example \
               zones.outdoor.example.json:zones.outdoor.json.example; do
  share homemind-rpi5-perception "$NATIVE/services/homemind-vision/config/${example%%:*}" "vision/${example##*:}"
done
# Partial-intent hints (off by default until measured on the Pi): libvosk is dlopen()ed, so ldd bundling misses it.
vosk_zip="$NATIVE/.deps/downloads/vosk-0.3.45/vosk-linux-aarch64-0.3.45.zip"
python3 -c "import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); open('/tmp/libvosk.so', 'wb').write(z.read('vosk-linux-aarch64-0.3.45/libvosk.so'))" "$vosk_zip"
mkdir -p /tmp/stage-homemind-rpi5-perception/lib
cp /tmp/libvosk.so /tmp/stage-homemind-rpi5-perception/lib/libvosk.so
cp -L /usr/lib/aarch64-linux-gnu/libatomic.so.1 /tmp/stage-homemind-rpi5-perception/lib/
share homemind-rpi5-perception "$W/services/edge-ai/partial-intent/artifacts/partial-intent-v2.onnx" models/partial-intent/partial-intent-v2.onnx
share homemind-rpi5-perception "$W/services/edge-ai/partial-intent/artifacts/partial-intent-v2.json" models/partial-intent/partial-intent-v2.json
# ALSA config fallback for a device without alsa-lib data (see rpi5-02 README); libasound itself is bundled.
share homemind-rpi5-perception /usr/share/alsa alsa
make_ipk homemind-rpi5-perception rpi5-02 homemind-whisper.service homemind-perception.service homemind-vision.service homemind-clock-sync.service homemind-clock-sync.timer
fi

if [[ " $nodes " == *" rpi4api "* ]]; then
echo "== NODE-RPI4-01 (Mobile API; Pi 4 = Cortex-A72, separate build tree)"
cmake -S "$NATIVE" -B /build/a72 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHM_BUILD_TESTS=ON   -DCMAKE_C_FLAGS="-mcpu=cortex-a72" -DCMAKE_CXX_FLAGS="-mcpu=cortex-a72" >/dev/null
cmake --build /build/a72 --target homemind-mobile-api homemind_mobile_api_tests hm_common_tests
if [[ $skip_tests -eq 0 ]]; then (cd /build/a72 && ctest --output-on-failure -R '^(hm_common|homemind_mobile_api)_tests$'); fi
# the packaged config turns HTTPS on: a build without OpenSSL headers must not become a package
/build/a72/services/homemind-mobile-api/homemind-mobile-api --version | grep -q '+tls' \
  || { echo "homemind-mobile-api was built without TLS (libssl-dev missing in the build image)" >&2; exit 1; }
stage homemind-rpi4-mobile-api /build/a72/services/homemind-mobile-api/homemind-mobile-api
# libmosquitto comes from homemind-core (same /data/share/homemind/lib); bundling it again would clash in opkg.
HM_BUNDLE_LIBS=0 make_ipk homemind-rpi4-mobile-api rpi4-mobile-api homemind-mobile-api.service
fi

if [[ " $nodes " == *" ui "* ]]; then
echo "== NODE-RPI5-01 (7-inch UI, flutter-elinux Wayland bundle)"
# Bundle with the absolute RUNPATH of the Atlas app folder (tool/elinux/build_elinux.sh INSTALL_RPATH).
ui_tar="${HM_UI_BUNDLE:-$NATIVE/.deps/ui-elinux/out/sony-3.29.3-atlas/homemind-ui-elinux-wayland-arm64.tar.gz}"
ui_dev="$W/team/homemind-core-ui-2026-10-20/homemind_flutter_test/tool/elinux/device"
ui_version="$(tr -d ' \r\n' < "$PKG/rpi5-01-ui/VERSION")"
rm -rf /tmp/stage-homemind-rpi5-ui && mkdir -p /tmp/stage-homemind-rpi5-ui/{bin,share,ui}
tar -xzf "$ui_tar" -C /tmp/stage-homemind-rpi5-ui/ui --strip-components=1
# Runs as the Atlas app com.atlas.app.homemind_ui (homemind-ui-atlas.sh); the plain Wayland launcher stays
# for a device whose compositor shows any client.
install -m 0755 "$PKG/rpi5-01-ui/homemind-ui-atlas.sh" /tmp/stage-homemind-rpi5-ui/ui/homemind-ui-atlas.sh
sed "s/@VERSION@/$ui_version/" "$PKG/rpi5-01-ui/appinfo.json" > /tmp/stage-homemind-rpi5-ui/ui/appinfo.json
install -m 0755 "$ui_dev/homemind-ui-launch.sh" /tmp/stage-homemind-rpi5-ui/ui/homemind-ui-launch.sh
chmod 0755 /tmp/stage-homemind-rpi5-ui/ui/homemind_flutter_test
readelf -d /tmp/stage-homemind-rpi5-ui/ui/homemind_flutter_test | grep -q 'RUNPATH.*/data/share/usr/atlas/apps/com.atlas.app.homemind_ui/lib' \
  || { echo "UI bundle without the Atlas app RUNPATH: rebuild with tool/elinux/build_elinux.sh" >&2; exit 1; }
make_ipk homemind-rpi5-ui rpi5-01-ui homemind-ui.service
fi

(cd "$OUT" && sha256sum homemind-*_aarch64.ipk > SHA256SUMS && cat SHA256SUMS)
