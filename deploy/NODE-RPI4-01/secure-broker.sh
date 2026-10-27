#!/usr/bin/env bash
# Close the Pi4 broker to the router network (2026-10-23: the router LAN is 10.10.10.0/24, so the old direct-cable
# anonymous listener 10.10.10.2:1883 became reachable over Wi-Fi). RUN ONLY AFTER THE USER APPROVES.
#   bash deploy/NODE-RPI4-01/secure-broker.sh [root@<rpi4-ip>] [--reset-accounts]
# From the laptop (macOS/Linux/Git Bash). Needs python3 on the laptop (hashing); the device has no mosquitto_passwd.
# A person types each password here (hidden); only the hashes reach the device.
# The same password goes later into that device's password file (Pi5, Mobile API) or secrets.h (ESP).
# Steps: backup → accounts (existing ones kept unless --reset-accounts) → mosquitto.conf: 1883 only on 127.0.0.1
#        (core), 9001 web preview removed, 1884 = password + mosquitto.acl → core drop-in --broker-host 127.0.0.1 →
#        restart broker (old config restored automatically if it does not come up) and core → checks.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
site="$here/../site.env"
if [[ -f "$site" ]]; then set -a; . "$site"; set +a; fi
reset=0
args=()
for a in "$@"; do [[ "$a" == "--reset-accounts" ]] && reset=1 || args+=("$a"); done
target="${args[0]:-${SSH_USER:-root}@${RPI4_01_HOST:-}}"
[[ "$target" != *@ ]] || { echo "ssh target missing: root@<rpi4-ip> or deploy/site.env" >&2; exit 2; }
host="${target#*@}"
. "$here/../common/payload.sh"
known_hosts="$here/../../team/homemind-core-ui-2026-10-20/.pi4_known_hosts"
ssh_opts=(-o UserKnownHostsFile="${HM_KNOWN_HOSTS:-$known_hosts}" -o StrictHostKeyChecking=yes)
if [[ -n "${HM_SSH_EXTRA:-}" ]]; then read -ra extra <<< "$HM_SSH_EXTRA"; ssh_opts+=("${extra[@]}"); fi
while read -r o; do ssh_opts+=("$o"); done < <(hm_ssh_shared_opts)  # one login per run (macOS/Linux)
on() { ssh "${ssh_opts[@]}" "$target" "$@"; }

# Hashes in the device broker's 2.0 format (hm_mosquitto_hash, payload.sh): Homebrew mosquitto_passwd 2.1 writes a
# format the Pi4's mosquitto 2.0.18 refuses.

HM=/data/share/homemind
C=$HM/config
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
accounts=(NODE-RPI5-01 NODE-RPI5-02 homemind-mobile-api CTRL-ESP32-ACT-01 CTRL-ESP8266-LED-01)
# Rollback source: the oldest broker backup that is not already this script's config (a rerun after a stop half-way
# must go back to the original), else this run's backup.
restore_conf="src=\$(for b in \$(ls -d $HM/backup/broker-* | sort); do grep -q secure-broker.sh \$b/mosquitto.conf || { echo \$b; break; }; done); [ -n \"\$src\" ] || src=$HM/backup/broker-$stamp; cat \$src/mosquitto.conf > $C/mosquitto.conf; chown network:network $C/mosquitto.conf; chmod 0640 $C/mosquitto.conf; echo \"  restored \$src/mosquitto.conf\""

echo "== 1. backup broker config, accounts, ACL and the core unit -> $HM/backup/broker-$stamp"
on "set -e; d=$HM/backup/broker-$stamp; mkdir -p \$d; cp -a $C/mosquitto.conf $C/mosquitto.passwd $C/mosquitto.acl \$d/; \
  cp -a /etc/systemd/system/homemind-core.service \$d/; ls \$d"

echo "== 2. accounts (passwords typed here, hidden; only hashes go to the device)"
existing="$(on "cut -d: -f1 $C/mosquitto.passwd")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
new="$work/passwd"
made=0
for user in "${accounts[@]}"; do
  if [[ $reset -eq 0 ]] && grep -qx "$user" <<< "$existing"; then echo "  $user: exists, kept"; continue; fi
  read -rs -p "  $user: password (hidden, remember it: it goes into that device later): " p; echo
  read -rs -p "  again: " q; echo
  if [[ -z "$p" || "$p" != "$q" || "$p" == *[[:space:]]* ]]; then
    unset p q; echo "  empty, not the same or with spaces: $user skipped (set it later with set-mqtt-password.sh)"; continue
  fi
  printf '%s\n' "$p" | hm_mosquitto_hash "$user" >> "$new"
  unset p q
  made=$((made + 1))
done
if [[ $made -gt 0 ]]; then
  on "umask 077; cat > /tmp/hm-passwd-new" < "$new"
  on "set -e; cd $C; awk -F: 'NR==FNR { add[\$1] = 1; next } !(\$1 in add)' /tmp/hm-passwd-new mosquitto.passwd > /tmp/hm-passwd-merged; \
    cat /tmp/hm-passwd-new >> /tmp/hm-passwd-merged; cat /tmp/hm-passwd-merged > mosquitto.passwd; \
    rm -f /tmp/hm-passwd-new /tmp/hm-passwd-merged; echo \"  accounts now: \$(cut -d: -f1 mosquitto.passwd | tr '\n' ' ')\""
fi

echo "== 3. broker listeners: 1883 local only (core), 1884 password + ACL, no 9001"
on "cat > $C/mosquitto.conf" <<'EOF'
per_listener_settings true

# Pi4-local anonymous listener: homemind-core only (secure-broker.sh, 2026-10-23). Not reachable from the router LAN.
listener 1883 127.0.0.1
allow_anonymous true

# Router LAN / Wi-Fi: every node with its own account (node ID) and the pfn.v1 ACL.
listener 1884 0.0.0.0
allow_anonymous false
password_file /data/share/homemind/config/mosquitto.passwd
acl_file /data/share/homemind/config/mosquitto.acl

persistence false
log_dest stdout
log_type error
log_type warning
log_type notice
EOF

echo "== 3b. the broker runs as 'network': owner network, mode 0640 (BusyBox cp makes root-owned copies: real Pi4, 2026-10-23)"
if ! on "cd $C && chown network:network mosquitto.conf mosquitto.passwd mosquitto.acl && chmod 0640 mosquitto.conf mosquitto.passwd mosquitto.acl   && stat -c '  %U %a %n' mosquitto.conf mosquitto.passwd mosquitto.acl   && [ \"\$(stat -c '%U%a' mosquitto.conf mosquitto.passwd mosquitto.acl | sort -u)\" = network640 ]"; then
  echo "  broker files are not readable by 'network': restoring the old broker config, nothing restarted" >&2
  on "$restore_conf" || true
  exit 1
fi

echo "== 4. core connects to 127.0.0.1:1883 (drop-in in /etc, where Atlas takes it at boot; / remounted rw briefly)"
# A drop-in only in /data/share/usr/lib/systemd/system worked until the next boot (units there were not used after
# a reboot of the real Pi5-02, 2026-10-23), so it goes to /etc like the packages' unit copies; the old /data copy goes.
E=/etc/systemd/system/homemind-core.service.d/20-local-broker.conf
D=/data/share/usr/lib/systemd/system/homemind-core.service.d/20-local-broker.conf
dropin="$(on "line=\$(grep '^ExecStart=' /etc/systemd/system/homemind-core.service | sed -n 1p | sed 's/--broker-host [^ ]*/--broker-host 127.0.0.1/'); \
  printf '[Service]\nExecStart=\n%s\n' \"\$line\"")"
if ! { [[ "$dropin" == *"--broker-host 127.0.0.1 "* ]] && printf '%s\n' "$dropin" | on "$(hm_etc_write_cmd $E)" \
       && on "rm -f $D; systemctl daemon-reload; systemctl cat homemind-core | grep '^ExecStart=/' | tail -1 \
              | grep -q -- '--broker-host 127.0.0.1 ' && echo '  core will use 127.0.0.1:1883 ($E)'"; }; then
  echo "  the core drop-in is not in effect: restoring the old broker config, nothing restarted" >&2
  printf '[Service]\n' | on "$(hm_etc_write_cmd $E)" || true  # an empty drop-in changes nothing
  on "$restore_conf; rm -f $D; systemctl daemon-reload" || true
  exit 1
fi

echo "== 5. restart broker (restores the old config if it does not come up), then core"
# active twice, 2 s apart: a broker that cannot read its files exits right after "Started"
if ! on "systemctl restart homemind-mqtt; sleep 3; systemctl is-active homemind-mqtt && sleep 2 && systemctl is-active homemind-mqtt"; then
  echo "  broker did not start: restoring the original broker config and the core unit" >&2
  printf '[Service]\n' | on "$(hm_etc_write_cmd $E)" || true  # an empty drop-in changes nothing
  on "$restore_conf; rm -f $D; \
    systemctl daemon-reload; systemctl restart homemind-mqtt homemind-core; sleep 2; \
    systemctl is-active homemind-mqtt; journalctl -u homemind-mqtt -n 15 --no-pager" || true
  exit 1
fi
on "systemctl restart homemind-core; sleep 5; systemctl is-active homemind-core; \
  journalctl -u homemind-core -n 20 --no-pager | grep -E 'CORE_STARTED|FATAL|MQTT_CONNECTED' | tail -2"

echo "== 6. checks"
on "(netstat -ltn 2>/dev/null || ss -ltn) | grep -E ':(1883|1884|9001) ' || true"
if (exec 3<>"/dev/tcp/$host/1883") 2>/dev/null; then echo "  FAIL: $host:1883 still open to the network" >&2; exit 1; fi
echo "  OK: $host:1883 closed to the network (core uses 127.0.0.1:1883)"
if (exec 3<>"/dev/tcp/$host/1884") 2>/dev/null; then echo "  OK: $host:1884 open (password + ACL)"; else echo "  FAIL: $host:1884 not reachable" >&2; exit 1; fi
echo "done. Backup: $HM/backup/broker-$stamp. Next: Pi5 / Mobile API password files with the same passwords."
