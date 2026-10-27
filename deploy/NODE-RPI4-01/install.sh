#!/usr/bin/env bash
# Upgrade NODE-RPI4-01 to homemind-core 0.6.5 (from 0.5.2, 0.6.3 or 0.6.4).  RUN ONLY AFTER THE USER APPROVES (CLAUDE.md).
# 0.6.5 = registry v4 config (door GPIO19 / window GPIO21 open-close motors, bathroom door removed, 2026-10-25) and a
# postinst that enables homemind-core again.
#   bash deploy/NODE-RPI4-01/install.sh root@<rpi4-ip>      (payload: bash deploy/collect.sh NODE-RPI4-01)
# Steps: backup the operating DB (never overwrite it) → copy IPK → opkg install → check services.
# Rollback: opkg install --force-downgrade payload/homemind-core_0.6.4_aarch64.ipk (or 0.6.3, 0.5.2), then restore nothing
# (schema 5 only adds columns; keep the backup).
set -euo pipefail

# Optional site values (deploy/site.env, see site.env.example): addresses only, used when no arguments are given.
site="$(cd "$(dirname "$0")/.." && pwd)/site.env"
if [[ -f "$site" ]]; then set -a; . "$site"; set +a; fi
if [[ $# -eq 0 && -n "${RPI4_01_HOST:-}" ]]; then set -- "${SSH_USER:-root}@$RPI4_01_HOST"; fi
target="${1:?ssh target, e.g. root@<rpi4-ip> (or fill deploy/site.env)}"
here="$(cd "$(dirname "$0")" && pwd)"
version=0.6.5
ipk="$here/payload/homemind-core_${version}_aarch64.ipk"
[[ -f "$ipk" ]] || { echo "no payload: run  bash deploy/collect.sh NODE-RPI4-01  first" >&2; exit 2; }
. "$here/../common/payload.sh"
payload_verify "$here/payload" || exit 1  # release signature, then every hash (the IPK included)
grep -qE "[ *]homemind-core_${version//./\.}_aarch64\.ipk$" "$here/payload/SHA256SUMS" || { echo "core $version not in the payload" >&2; exit 1; }
known_hosts="$here/../../team/homemind-core-ui-2026-10-20/.pi4_known_hosts"  # pinned Pi4 host key (team file)
ssh_opts=(-o UserKnownHostsFile="${HM_KNOWN_HOSTS:-$known_hosts}" -o StrictHostKeyChecking=yes)
# Test hook only (deploy/test/run-install-dryrun.sh): extra ssh/scp options such as -o Port=2222 -i <key>.
if [[ -n "${HM_SSH_EXTRA:-}" ]]; then read -ra extra <<< "$HM_SSH_EXTRA"; ssh_opts+=("${extra[@]}"); fi
while read -r o; do ssh_opts+=("$o"); done < <(hm_ssh_shared_opts)  # one login per run (macOS/Linux)
stamp="$(date -u +%Y%m%dT%H%M%SZ)"

echo "== 1. current state"
ssh "${ssh_opts[@]}" "$target" 'opkg list-installed | grep homemind-core; systemctl is-active homemind-core homemind-mqtt; df -h /data | tail -1'

echo "== 2. backup operating DB (core stopped only for the file copy, then started again; original untouched)"
echo "      (the online sqlite3 .backup starts over whenever core writes; it hung on the real Pi4, 2026-10-25)"
ssh "${ssh_opts[@]}" "$target" "set -e; B=/data/share/homemind/backup; D=/data/share/homemind/db; mkdir -p \$B; \
  systemctl stop homemind-core; rc=0; \
  for s in '' -wal -shm; do if [ -f \$D/homemind.db\$s ]; then cp -a \$D/homemind.db\$s \$B/homemind-$stamp.db\$s || rc=1; fi; done; \
  systemctl start homemind-core; [ \$rc -eq 0 ]; \
  cp -a /data/share/homemind/config \$B/config-$stamp; ls -la \$B | tail -4"

echo "== 3. install $version"
hm_scp "${ssh_opts[@]}" "$ipk" "$target:/tmp/homemind-core_${version}_aarch64.ipk"
ssh "${ssh_opts[@]}" "$target" "opkg install /tmp/homemind-core_${version}_aarch64.ipk"


echo "== 4. contract configs (opkg keeps changed conffiles and puts the package's copy at <file>-opkg; core 0.6.x"
echo "      refuses the older ones, e.g. 0.5.0's registry v2 on the real Pi4 2026-10-23). Old copies: backup/config-$stamp"
echo "      (cat > keeps the file's owner; BusyBox cp would make it root-owned)"
ssh "${ssh_opts[@]}" "$target" 'cd /data/share/homemind/config && for f in device-registry scenario-rules sensor-rules task-policy context-rules; do if [ -f "$f.json-opkg" ]; then cat "$f.json-opkg" > "$f.json" && rm "$f.json-opkg" && echo "  replaced $f.json with the package version"; fi; printf "%s " "$f"; grep -o "\"\(registry\|rules\|policy\)_version\": [0-9]*" "$f.json" || echo MISSING; done; ls *-opkg 2>/dev/null || true'

echo "== 5. start at boot (the 0.6.3/0.6.4 prerm disables homemind-core on upgrade; the 0.6.4 postinst did not enable it"
echo "      again: real Pi4 2026-10-25, core inactive after a power cycle. 0.6.5's postinst does; kept here as well)"
ssh "${ssh_opts[@]}" "$target" "$(hm_enable_cmd homemind-core homemind-mqtt)"

echo "== 6. services and logs"
ssh "${ssh_opts[@]}" "$target" 'systemctl restart homemind-core; sleep 5; systemctl is-active homemind-core homemind-mqtt; journalctl -u homemind-core -n 20 --no-pager | grep -E "CORE_STARTED|FATAL|MQTT_CONNECTED" | tail -3 || true'
echo "done: backup homemind-$stamp.db kept on the device"
