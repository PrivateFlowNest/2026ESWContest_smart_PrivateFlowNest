#!/usr/bin/env bash
# Set one node's MQTT password from the laptop, in both places at once so they cannot differ (2026-10-23: two
# separate typings on the real Pi4 gave "not authorised"):
#   1. the broker account on NODE-RPI4-01 (hash in the device broker's 2.0 format made here by hm_mosquitto_hash,
#      merged into mosquitto.passwd, broker reloads its files with SIGHUP, no restart)
#   2. the node's password file (root:network 0640), then its services restart
# The password is typed here (hidden). It is never stored on the laptop, never an argument, never logged; it
# reaches the devices only through SSH stdin. Needs python3 on the laptop (hashing), not mosquitto_passwd.
#   bash deploy/set-mqtt-password.sh mobile-api     (NODE-RPI4-01, account homemind-mobile-api)
#   bash deploy/set-mqtt-password.sh rpi5-01        (NODE-RPI5-01, account NODE-RPI5-01)
#   bash deploy/set-mqtt-password.sh rpi5-02        (NODE-RPI5-02, account NODE-RPI5-02)
#   bash deploy/set-mqtt-password.sh esp32|esp8266  (broker account only: the password goes into secrets.h)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[[ -f "$here/site.env" ]] || { echo "no deploy/site.env (addresses)" >&2; exit 2; }
set -a; . "$here/site.env"; set +a
. "$here/common/payload.sh"
HM=/data/share/homemind
C=$HM/config
pi4_known="$here/../team/homemind-core-ui-2026-10-20/.pi4_known_hosts"
host=""; file=""; units=""; known="$here/.known_hosts"; strict=accept-new
case "${1:-}" in
  mobile-api) host="${RPI4_01_HOST:-}"; account=homemind-mobile-api; file=$HM/secrets/mobile-api-mqtt-password
              units="homemind-mobile-api"; known="$pi4_known"; strict=yes ;;
  rpi5-01)    host="${RPI5_01_HOST:-}"; account=NODE-RPI5-01; file=$HM/secrets/mqtt-password; units="homemind-llm homemind-ui" ;;
  rpi5-02)    host="${RPI5_02_HOST:-}"; account=NODE-RPI5-02; file=$HM/secrets/mqtt-password
              units="homemind-perception homemind-vision" ;;
  esp32)      account=CTRL-ESP32-ACT-01 ;;
  esp8266)    account=CTRL-ESP8266-LED-01 ;;
  *) sed -n '2,12p' "$0" >&2; exit 2 ;;
esac
[[ -n "${RPI4_01_HOST:-}" ]] || { echo "no RPI4_01_HOST in deploy/site.env" >&2; exit 2; }
[[ -z "$file" || -n "$host" ]] || { echo "no address for $1 in deploy/site.env" >&2; exit 2; }
shared=()
while read -r o; do shared+=("$o"); done < <(hm_ssh_shared_opts)
pi4=(-o UserKnownHostsFile="$pi4_known" -o StrictHostKeyChecking=yes "${shared[@]}")
node=(-o UserKnownHostsFile="$known" -o StrictHostKeyChecking="$strict" "${shared[@]}")

read -rs -p "new password of MQTT account $account (hidden): " p; echo
read -rs -p "again: " q; echo
[[ -n "$p" && "$p" == "$q" ]] || { unset p q; echo "empty or not the same: nothing changed" >&2; exit 1; }
unset q
[[ "$p" != *[[:space:]]* ]] || { unset p; echo "no spaces please (the devices trim them): nothing changed" >&2; exit 1; }

echo "== 1. broker account $account on NODE-RPI4-01 (${RPI4_01_HOST})"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '%s\n' "$p" | hm_mosquitto_hash "$account" > "$work/passwd"  # device broker 2.0 format (payload.sh)
ssh "${pi4[@]}" "root@$RPI4_01_HOST" "set -e; cd $C; umask 077; cat > /tmp/hm-passwd-new; \
  awk -F: 'NR==FNR { add[\$1] = 1; next } !(\$1 in add)' /tmp/hm-passwd-new mosquitto.passwd > /tmp/hm-passwd-merged; \
  cat /tmp/hm-passwd-new >> /tmp/hm-passwd-merged; cat /tmp/hm-passwd-merged > mosquitto.passwd; \
  rm -f /tmp/hm-passwd-new /tmp/hm-passwd-merged; chown network:network mosquitto.passwd; chmod 0640 mosquitto.passwd; \
  systemctl kill -s HUP homemind-mqtt; echo '  hash replaced, broker reloaded its password file'" < "$work/passwd"

if [[ -n "$file" ]]; then
  echo "== 2. password file on $host: $file"
  # Only basic BusyBox applets: the real Pi4 (Atlas) has no `install`. umask 077 keeps the file private until chown.
  printf '%s\n' "$p" | ssh "${node[@]}" "root@$host" "set -e; umask 077; mkdir -p $HM/secrets; chown root:network $HM/secrets; \
    chmod 0750 $HM/secrets; cat > $file.new; chown root:network $file.new; chmod 0640 $file.new; mv $file.new $file; \
    ls -l $file | awk '{print \"  \" \$1, \$3, \$4, \$NF}'"
  unset p
  echo "== 3. restart $units"
  ssh "${node[@]}" "root@$host" "for u in $units; do systemctl restart \$u 2>/dev/null || echo \"  \$u: not installed yet\"; done; sleep 6; \
    for u in $units; do printf '  %s ' \$u; systemctl is-active \$u || true; done; \
    for u in $units; do journalctl -u \$u -n 30 --no-pager | grep -iE 'mqtt (connected|connect refused)|not authori' | tail -n 1; done"
else
  unset p
  echo "== 2. put the same password into firmware/include/secrets.h for $account (never commit it)"
fi
