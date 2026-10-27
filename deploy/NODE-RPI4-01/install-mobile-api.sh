#!/usr/bin/env bash
# Install the phone Mobile API on NODE-RPI4-01 (after core 0.6.3). RUN ONLY AFTER THE USER APPROVES.
#   bash deploy/NODE-RPI4-01/install-mobile-api.sh root@<rpi4-ip> [--broker-host 127.0.0.1] [--broker-port 1884]
#   bash deploy/NODE-RPI4-01/install-mobile-api.sh            (addresses from deploy/site.env)
# Payload: bash deploy/collect.sh NODE-RPI4-01. The MQTT password (account homemind-mobile-api) and the phone pairing
# are done by a person on the device; this script prints how. It never handles passwords, codes or tokens.
set -euo pipefail
site="$(cd "$(dirname "$0")/.." && pwd)/site.env"
if [[ -f "$site" ]]; then set -a; . "$site"; set +a; fi
here="$(cd "$(dirname "$0")" && pwd)"
if [[ $# -eq 0 && -n "${RPI4_01_HOST:-}" ]]; then set -- "${SSH_USER:-root}@$RPI4_01_HOST"; fi
target="${1:?ssh target, e.g. root@<rpi4-ip> (or fill deploy/site.env)}"; shift
host_ip="${target#*@}"
broker_host=127.0.0.1; broker_port="${BROKER_PORT:-1884}"   # the broker runs on this same Pi 4
while [[ $# -gt 0 ]]; do
  case "$1" in
    --broker-host) broker_host="$2"; shift 2;;
    --broker-port) broker_port="$2"; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
payload="$here/payload"
[[ -f "$payload/SHA256SUMS" ]] || { echo "no payload: run  bash deploy/collect.sh NODE-RPI4-01  first" >&2; exit 2; }
. "$here/../common/payload.sh"
payload_verify "$payload" || exit 1  # release signature, then every hash
ipk="$(ls "$payload"/homemind-rpi4-mobile-api_*_aarch64.ipk | sort -V | tail -1)"
known_hosts="$here/../../team/homemind-core-ui-2026-10-20/.pi4_known_hosts"  # pinned Pi4 host key (team file)
ssh_opts=(-o UserKnownHostsFile="${HM_KNOWN_HOSTS:-$known_hosts}" -o StrictHostKeyChecking=yes)
# Test hook only (deploy/test/run-install-dryrun.sh): extra ssh/scp options such as -o Port=2222 -i <key>.
if [[ -n "${HM_SSH_EXTRA:-}" ]]; then read -ra extra <<< "$HM_SSH_EXTRA"; ssh_opts+=("${extra[@]}"); fi
while read -r o; do ssh_opts+=("$o"); done < <(hm_ssh_shared_opts)  # one login per run (macOS/Linux)
HM=/data/share/homemind

echo "== 1. core must be 0.6.3 or newer (ACL: 7-inch UI pairing QR 0.6.2, phone approvals 0.6.3)"
ssh "${ssh_opts[@]}" "$target" "opkg list-installed | grep -E '^homemind-core - 0\.6\.[3-9]|^homemind-core - 0\.([7-9]|[1-9][0-9])\.' \
  || { echo 'install core 0.6.3 first (deploy/NODE-RPI4-01/install.sh)'; exit 1; }"

acl="$payload/mosquitto.acl"
if [[ -f "$acl" ]]; then
  # The core 0.6.3/0.6.4 template (on the real Pi4 since 2026-10-23). Any other ACL was edited by hand: not replaced.
  old_acl=0e06d2f5598b7118f62a0e4ba3b43723352d2874d22cd227dc7d9ef53b77b7d5
  new_acl="$(hm_sha256 "$acl")"
  echo "== 1b. broker ACL: 7-inch UI phone list (pfn/v1/mobile/phones/*), reloaded without a restart"
  ssh "${ssh_opts[@]}" "$target" "set -e; C=$HM/config; cur=\$(sha256sum \$C/mosquitto.acl | cut -d' ' -f1); \
    if [ \"\$cur\" = $new_acl ]; then echo '  ACL already current'; cat > /dev/null; exit 0; fi; \
    if [ \"\$cur\" != $old_acl ]; then echo '  the ACL on the device is not the core template: not replaced'; cat > /dev/null; exit 1; fi; \
    mkdir -p $HM/backup; b=$HM/backup/mosquitto.acl-\$(date -u +%Y%m%dT%H%M%SZ); cp -a \$C/mosquitto.acl \$b; \
    cat > \$C/mosquitto.acl.new; chown network:network \$C/mosquitto.acl.new; chmod 0640 \$C/mosquitto.acl.new; \
    [ \"\$(sha256sum \$C/mosquitto.acl.new | cut -d' ' -f1)\" = $new_acl ] || { rm -f \$C/mosquitto.acl.new; echo '  copy damaged'; exit 1; }; \
    mv \$C/mosquitto.acl.new \$C/mosquitto.acl; systemctl kill -s HUP homemind-mqtt; sleep 2; \
    systemctl is-active homemind-mqtt; echo \"  ACL replaced (old copy \$b), broker reloaded it\"" < "$acl"
fi

echo "== 2. install $(basename "$ipk")"
hm_scp "${ssh_opts[@]}" "$ipk" "$target:/tmp/$(basename "$ipk")"
ssh "${ssh_opts[@]}" "$target" "opkg install --force-reinstall /tmp/$(basename "$ipk") && rm -f /tmp/$(basename "$ipk")"
echo "   start at boot (the 0.1.0 postinst could not enable it on the read-only /)"
ssh "${ssh_opts[@]}" "$target" "$(hm_enable_cmd homemind-mobile-api)"

echo "== 3. addresses in mobile-api.json (template placeholders only)"
ssh "${ssh_opts[@]}" "$target" "cd $HM/config && sed -i -e 's/\"host\": \"BROKER_HOST\"/\"host\": \"$broker_host\"/' \
  -e 's/\"port\": 1884,/\"port\": $broker_port,/' -e 's#https://RPI4_01_ADDRESS:8000#https://$host_ip:8000#' mobile-api.json && \
  grep -E '\"host\"|\"port\"|public_base_url' mobile-api.json"

echo "== 4. MQTT password file (account homemind-mobile-api)"
if ! ssh "${ssh_opts[@]}" "$target" "test -s $HM/secrets/mobile-api-mqtt-password"; then
  echo "MISSING. From this laptop (same password as the homemind-mobile-api account of secure-broker.sh):"
  echo "  bash deploy/set-mqtt-password.sh mobile-api"
fi

echo "== 5. service"
ssh "${ssh_opts[@]}" "$target" "sleep 3; systemctl is-active homemind-mobile-api; journalctl -u homemind-mobile-api -n 8 --no-pager"
echo "Pair a phone (on the device, the QR shows in the terminal):"
echo "  7-inch UI: More -> phone connect, or from this laptop:"
echo "  ssh -t root@$host_ip $HM/bin/homemind-mobile-api --config $HM/config/mobile-api.json --print-pairing-qr"
