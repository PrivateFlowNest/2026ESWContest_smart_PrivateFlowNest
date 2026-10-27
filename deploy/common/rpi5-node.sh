#!/usr/bin/env bash
# Shared installer for the two RPI5 nodes; called by deploy/NODE-RPI5-0X/install.sh.
# RUN ONLY AFTER THE USER APPROVES (CLAUDE.md: no unverified IPK on devices).
#   rpi5-node.sh <01|02> <payload dir> <ssh target> --broker-host <rpi4-ip> [--broker-port 1884]
# Steps: verify payload hashes -> show device state -> copy model(s) if missing/different -> opkg install ->
#        set broker address in the node config (template placeholder only) -> check the MQTT password file -> services.
# The MQTT password is never handled here: it is created on the device by a person (instructions printed).
set -euo pipefail

node="${1:?node 01 or 02}"; payload="${2:?payload dir}"; target="${3:?ssh target}"; shift 3
broker_host=""; broker_port=1884
while [[ $# -gt 0 ]]; do
  case "$1" in
    --broker-host) broker_host="$2"; shift 2;;
    --broker-port) broker_port="$2"; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
[[ -n "$broker_host" ]] || { echo "--broker-host is required (RPI4-01 address on the router)" >&2; exit 2; }
[[ -f "$payload/SHA256SUMS" ]] || { echo "no payload: run  bash deploy/collect.sh NODE-RPI5-$node  first" >&2; exit 2; }
. "$(cd "$(dirname "$0")" && pwd)/payload.sh"
payload_verify "$payload" || exit 1  # release signature, then every hash

here="$(cd "$(dirname "$0")" && pwd)"
ssh_opts=(-o UserKnownHostsFile="${HM_KNOWN_HOSTS:-$here/../.known_hosts}")
# Test hook only (deploy/test/run-install-dryrun.sh): extra ssh/scp options such as -o Port=2222 -i <key>.
if [[ -n "${HM_SSH_EXTRA:-}" ]]; then read -ra extra <<< "$HM_SSH_EXTRA"; ssh_opts+=("${extra[@]}"); fi
while read -r o; do ssh_opts+=("$o"); done < <(hm_ssh_shared_opts)  # one login per run (macOS/Linux)
HM=/data/share/homemind
case "$node" in
  01) package=homemind-rpi5-llm; config=rpi5-01.json; units="homemind-cpu-limit homemind-exaone homemind-llm"
      models=("exaone-1.2b-homemind-v3-q4_k_m.gguf:exaone-1.2b-homemind-q4_k_m.gguf");;
  02) package=homemind-rpi5-perception; config=rpi5-02.json; units="homemind-whisper homemind-perception"
      models=("ggml-whisper-small-homemind-lora-v4-q5_0.bin:ggml-whisper-small-homemind-lora-v4-q5_0.bin");;
  *) echo "node must be 01 or 02" >&2; exit 2;;
esac
ipk="$(ls "$payload"/${package}_*_aarch64.ipk | sort -V | tail -1)"

echo "== 1. device state"
ssh "${ssh_opts[@]}" "$target" "uname -m; head -n 3 /etc/os-release; df -h /data | tail -1; free -m | head -n 2; \
  opkg list-installed | grep homemind || true; ls /usr/share/alsa >/dev/null 2>&1 && echo alsa-config:yes || echo alsa-config:no; \
  id network; getent group audio || echo 'no audio group'; date -u"

echo "== 2. models"
for pair in "${models[@]}"; do
  src="$payload/${pair%%:*}"; dst="$HM/models/${pair##*:}"
  sha="$(hm_sha256 "$src")"
  remote_sha="$(ssh "${ssh_opts[@]}" "$target" "sha256sum $dst 2>/dev/null | cut -d' ' -f1" || true)"
  if [[ "$remote_sha" == "$sha" ]]; then echo "  $dst up to date"; continue; fi
  ssh "${ssh_opts[@]}" "$target" "mkdir -p $HM/models"
  hm_scp "${ssh_opts[@]}" "$src" "$target:$dst.part"
  ssh "${ssh_opts[@]}" "$target" "echo '$sha  $dst.part' | sha256sum -c - && mv $dst.part $dst"
done
if [[ "$node" == 02 && -d "$payload/vosk-model-small-ko-0.22" ]]; then
  echo "  Vosk model (partial-intent hints stay off in rpi5-02.json until enabled)"
  tar -C "$payload" -cf - vosk-model-small-ko-0.22 | ssh "${ssh_opts[@]}" "$target" "mkdir -p $HM/models && tar -C $HM/models -xf -"
fi

echo "== 3. install $(basename "$ipk")"
hm_scp "${ssh_opts[@]}" "$ipk" "$target:/tmp/$(basename "$ipk")"
ssh "${ssh_opts[@]}" "$target" "opkg install --force-reinstall /tmp/$(basename "$ipk") && rm -f /tmp/$(basename "$ipk")"
if [[ "$node" == 01 ]]; then
  ui_ipk="$(ls "$payload"/homemind-rpi5-ui_*_aarch64.ipk 2>/dev/null | sort -V | tail -1 || true)"
  if [[ -n "$ui_ipk" ]]; then
    echo "== 3b. install $(basename "$ui_ipk") (7-inch UI, runs as the Atlas app com.atlas.app.homemind_ui)"
    hm_scp "${ssh_opts[@]}" "$ui_ipk" "$target:/tmp/$(basename "$ui_ipk")"
    # Runtime drop-ins from the 2026-10-23 display tests (/run) must not override the packaged unit.
    ssh "${ssh_opts[@]}" "$target" "rm -rf /run/systemd/system/homemind-ui.service.d && systemctl daemon-reload && \
      opkg install --force-reinstall /tmp/$(basename "$ui_ipk") && rm -f /tmp/$(basename "$ui_ipk")"
    ssh "${ssh_opts[@]}" "$target" "cd $HM/config && \
      sed -i -e 's/^HOMEMIND_PI4_HOST=BROKER_HOST\$/HOMEMIND_PI4_HOST=$broker_host/' -e 's/^HOMEMIND_MQTT_TCP_PORT=[0-9]*\$/HOMEMIND_MQTT_TCP_PORT=$broker_port/' ui.env && \
      { grep -q '^HOMEMIND_CAMERA_PREVIEW_URL=' ui.env || echo 'HOMEMIND_CAMERA_PREVIEW_URL=http://${RPI5_02_HOST:-10.10.10.4}:8091' >> ui.env; } &&       grep -E '^HOMEMIND_(PI4_HOST|MQTT_TCP_PORT|CAMERA_PREVIEW_URL)=' ui.env && systemctl restart homemind-ui"
    units="$units homemind-ui"
  fi
fi
echo "== 3c. start at boot ($units; the 0.1.0 postinst could not enable them on the read-only /)"
boot_units="$units homemind-clock-sync.service homemind-clock-sync.timer"  # clock from core (both nodes)
[[ "$node" == 02 ]] && boot_units="$boot_units homemind-vision"  # stays off until its config exists
ssh "${ssh_opts[@]}" "$target" "$(hm_enable_cmd $boot_units)"

echo "== 4. broker address in $config (only replaces the template placeholder)"
ssh "${ssh_opts[@]}" "$target" "cd $HM/config && \
  sed -i -e 's/\"host\": \"BROKER_HOST\"/\"host\": \"$broker_host\"/' -e 's/\"port\": [0-9]*/\"port\": $broker_port/' $config && \
  grep -A3 '\"broker\"' $config"

echo "== 5. MQTT password file"
if ! ssh "${ssh_opts[@]}" "$target" "test -s $HM/secrets/mqtt-password"; then
  echo "MISSING. From this laptop (same password as the NODE-RPI5-$node account of secure-broker.sh):"
  echo "  bash deploy/set-mqtt-password.sh rpi5-$node"
fi

echo "== 6. services (clock: homemind-clock-sync takes core's time 20 s after boot and every 10 min)"
ssh "${ssh_opts[@]}" "$target" "sleep 5; for u in $units homemind-clock-sync.timer; do printf '%s ' \$u; systemctl is-active \$u; done; \
  for u in $units; do journalctl -u \$u -n 8 --no-pager; done; journalctl -u homemind-clock-sync -n 3 --no-pager"
