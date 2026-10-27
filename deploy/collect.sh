#!/usr/bin/env bash
# Gather what each device needs into deploy/<DEVICE>/payload/ (gitignored), checking every file first:
# IPKs against their dist SHA256SUMS, models against docs/handoff/MODEL_MANIFEST.md. The payload's SHA256SUMS is then
# signed with the release key (deploy/common/payload.sh; installers refuse unsigned payloads). After this each device
# folder is self-contained (README + install script + payload) and can be copied to a USB stick as is.
#   bash deploy/collect.sh all                       # every device
#   bash deploy/collect.sh NODE-RPI5-02 --with-vosk-model
# Runs on the PC (Git Bash or Linux). Copies ~1.2 GB for all devices (EXAONE 812 MB, Whisper ~190 MB).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
manifest="$repo/docs/handoff/MODEL_MANIFEST.md"
. "$here/common/payload.sh"
target="${1:?device name or all}"; shift
with_vosk=0
[[ "${1:-}" == "--with-vosk-model" ]] && with_vosk=1

ipk_from() {  # ipk_from <dist dir> <file name> <payload dir>
  local dist=$1 name=$2 out=$3
  [[ -f "$dist/$name" ]] || { echo "missing $dist/$name (build it first, see the device README)" >&2; exit 1; }
  (cd "$dist" && grep " $name\$" SHA256SUMS | sha256sum -c --quiet -) || { echo "hash mismatch: $name" >&2; exit 1; }
  cp "$dist/$name" "$out/"
}

model_from() {  # model_from <source file> <payload dir> [<name in payload>]
  local src=$1 out=$2 name=${3:-$(basename "$1")}
  [[ -f "$src" ]] || { echo "missing model $src" >&2; exit 1; }
  local sha; sha="$(sha256sum "$src" | cut -d' ' -f1)"
  grep -q "$sha" "$manifest" || { echo "not in MODEL_MANIFEST.md: $src" >&2; exit 1; }
  # A hard link first: no bytes are copied. On the Windows PC two copies of the 812 MB EXAONE model in a row each came
  # out with one flipped bit (2026-10-25, source hash unchanged), so a copy is only the fallback and is checked.
  local try
  for try in 1 2 3; do
    rm -f "$out/$name"
    ln "$src" "$out/$name" 2>/dev/null || cp "$src" "$out/$name"
    [[ "$(sha256sum "$out/$name" | cut -d' ' -f1)" == "$sha" ]] && return 0
    echo "  copy of $name damaged (try $try)" >&2
  done
  echo "copy damaged three times: $out/$name" >&2
  exit 1
}

latest() { ls "$1"/$2 2>/dev/null | sort -V | tail -1 | xargs -r basename; }

prepare() {  # prepare <device> -> prints payload dir
  local out="$here/$1/payload"
  rm -rf "$out"; mkdir -p "$out"
  echo "$out"
}

finish() {  # finish <payload dir>
  (cd "$1" && find . -type f ! -name SHA256SUMS ! -name SHA256SUMS.sig | sed 's|^\./||' | sort \
    | xargs -d '\n' sha256sum > SHA256SUMS)
  payload_sign "$1"
  echo "  $(du -sh "$1" | cut -f1)  $1"
  sed 's/^/    /' "$1/SHA256SUMS"
}

collect_device() {
  local device=$1 out
  echo "== $device"
  case "$device" in
    NODE-RPI4-01)
      out="$(prepare "$device")"
      local core_dist="$repo/team/homemind-core-ui-2026-10-20/dist-ipk"
      ipk_from "$core_dist" homemind-core_0.6.5_aarch64.ipk "$out"
      ipk_from "$core_dist" homemind-core_0.6.4_aarch64.ipk "$out"   # rollback package (on the real Pi4 since 2026-10-24)
      ipk_from "$core_dist" homemind-core_0.6.3_aarch64.ipk "$out"   # rollback package (installed 2026-10-23)
      ipk_from "$core_dist" homemind-core_0.5.2_aarch64.ipk "$out"   # rollback package (team baseline)
      ipk_from "$repo/native/dist-ipk" "$(latest "$repo/native/dist-ipk" 'homemind-rpi4-mobile-api_*_aarch64.ipk')" "$out"
      # Broker ACL with the 7-inch phone list topics (2026-10-25); install-mobile-api.sh puts it on the device.
      cp "$repo/team/homemind-core-ui-2026-10-20/package/mosquitto.acl" "$out/mosquitto.acl"
      finish "$out";;
    NODE-RPI5-01)
      out="$(prepare "$device")"
      ipk_from "$repo/native/dist-ipk" "$(latest "$repo/native/dist-ipk" 'homemind-rpi5-llm_*_aarch64.ipk')" "$out"
      ipk_from "$repo/native/dist-ipk" "$(latest "$repo/native/dist-ipk" 'homemind-rpi5-ui_*_aarch64.ipk')" "$out"
      model_from "$repo/services/exaone-edge/artifacts/exaone-1.2b-homemind-v3-q4_k_m.gguf" "$out"
      finish "$out";;
    NODE-RPI5-02)
      out="$(prepare "$device")"
      ipk_from "$repo/native/dist-ipk" "$(latest "$repo/native/dist-ipk" 'homemind-rpi5-perception_*_aarch64.ipk')" "$out"
      model_from "$repo/services/edge-ai/speech-to-text/artifacts/training/whisper-small-homemind-lora-v4/ggml-whisper-small-homemind-lora-v4-q5_0.bin" "$out"
      if [[ $with_vosk == 1 ]]; then
        local vosk="$repo/services/edge-ai/speech-to-text/models/vosk-model-small-ko-0.22"
        (cd "$vosk/.." && find vosk-model-small-ko-0.22 -type f -size +2000k | while read -r f; do
           grep -q "$(sha256sum "$f" | cut -d' ' -f1)" "$manifest" || { echo "not in MODEL_MANIFEST.md: $f" >&2; exit 1; }
         done)
        cp -r "$vosk" "$out/"
      fi
      finish "$out";;
    PHONE-APP)
      out="$here/$device/payload"
      if [[ -f "$out/SHA256SUMS" ]]; then
        (cd "$out" && sha256sum -c --quiet SHA256SUMS) || { echo "hash mismatch in $out" >&2; exit 1; }
        [[ -f "$out/SHA256SUMS.sig" ]] || payload_sign "$out"
        sed 's/^/    /' "$out/SHA256SUMS"
      else
        echo "  no APK yet: bash deploy/PHONE-APP/build-apk.sh (Flutter + Android SDK on this PC)"
      fi;;
    CTRL-ESP32-ACT-01|CTRL-ESP8266-LED-01|DOLL-AXIS)
      echo "  firmware is built and flashed from firmware/ on the PC (see $device/README.md); nothing to collect";;
    NODE-RPI4-02)
      echo "  on hold (one-axis plan, see NODE-RPI4-02/README.md)";;
    *) echo "unknown device $device" >&2; exit 2;;
  esac
}

if [[ "$target" == all ]]; then
  for device in NODE-RPI4-01 NODE-RPI5-01 NODE-RPI5-02 PHONE-APP CTRL-ESP32-ACT-01 CTRL-ESP8266-LED-01 DOLL-AXIS                 NODE-RPI4-02; do
    collect_device "$device"
  done
else
  collect_device "$target"
fi
