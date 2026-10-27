#!/usr/bin/env bash
# Dry run of the deploy/ install scripts against "fake device" containers (PC simulation, NOT a Pi, NOT Atlas):
# real ssh/scp, real IPK unpack + postinst (fake opkg), stubbed systemctl. Checks the scripts' order, hash checks,
# model copies, config placeholder replacement, file modes, the missing-password notice and the DB backup.
#   bash deploy/collect.sh all && bash deploy/test/run-install-dryrun.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
deploy="$(cd "$here/.." && pwd)"
export MSYS_NO_PATHCONV=1   # keep container paths (/data/...) as they are on Git Bash
host_path() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
docker build -q -t homemind-fake-device -f "$(host_path "$here/fake-device.Dockerfile")" "$(host_path "$here")" >/dev/null
work="$(mktemp -d)"; trap 'rm -rf "$work"; docker rm -f hm-fake-rpi4 hm-fake-rpi5-01 hm-fake-rpi5-02 >/dev/null 2>&1 || true' EXIT
ssh-keygen -q -t ed25519 -N "" -f "$work/key"
fails=0
check() { if eval "$2"; then echo "  PASS $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }

start_device() {  # start_device <name> <port>
  docker rm -f "$1" >/dev/null 2>&1 || true
  docker run -d --name "$1" -p "127.0.0.1:$2:22" homemind-fake-device >/dev/null
  docker cp "$(host_path "$work/key.pub")" "$1:/root/.ssh/authorized_keys"
  docker exec "$1" sh -c 'chown root:root /root/.ssh/authorized_keys; chmod 600 /root/.ssh/authorized_keys'
  for _ in $(seq 1 30); do ssh-keyscan -p "$2" 127.0.0.1 >> "$work/known_hosts" 2>/dev/null && break; sleep 1; done
}
on() { docker exec "$1" sh -c "$2"; }
export HM_KNOWN_HOSTS="$work/known_hosts"

echo "== NODE-RPI4-01 (core 0.5.2 -> 0.6.5, existing DB)"
start_device hm-fake-rpi4 2201
on hm-fake-rpi4 'mkdir -p /data/share/homemind/db /data/share/homemind/config && sqlite3 /data/share/homemind/db/homemind.db "create table t(x); insert into t values(42);" && echo "homemind-core - 0.5.2" > /var/lib/fake-opkg/installed'
HM_SSH_EXTRA="-o Port=2201 -i $work/key" bash "$deploy/NODE-RPI4-01/install.sh" root@127.0.0.1 > "$work/rpi4.log" 2>&1 || { cat "$work/rpi4.log"; exit 1; }
check "DB backup made and original untouched" "on hm-fake-rpi4 'ls /data/share/homemind/backup/homemind-*.db >/dev/null && sqlite3 /data/share/homemind/db/homemind.db \"select x from t\" | grep -qx 42'"
check "core 0.6.5 installed" "on hm-fake-rpi4 'grep -q \"homemind-core - 0.6.5\" /var/lib/fake-opkg/installed'"
check "registry v4 in config (door GPIO19, window GPIO21)" "on hm-fake-rpi4 'grep -q GPIO19 /data/share/homemind/config/device-registry.json && grep -q GPIO21 /data/share/homemind/config/device-registry.json && grep -q OPEN_CLOSE_SERVO /data/share/homemind/config/device-registry.json'"
check "task policy v3 (proposal expiry) in config" "on hm-fake-rpi4 'grep -q proposal_ttl_seconds /data/share/homemind/config/task-policy.json && grep -q PROPOSAL_EXPIRED /data/share/homemind/config/task-policy.json'"
check "ACL template has voice/listen" "on hm-fake-rpi4 'grep -q ai/voice/listen /data/share/homemind/package-defaults/mosquitto.acl'"
check "unit copied to /etc by postinst" "on hm-fake-rpi4 'test -f /etc/systemd/system/homemind-core.service'"
check "core enabled at boot after the upgrade (the 0.6.3 prerm disables it)" "on hm-fake-rpi4 'grep -q \"^enable homemind-core homemind-mqtt\" /var/log/fake-systemctl.log'"
echo "== NODE-RPI4-01 Mobile API (after core)"
HM_SSH_EXTRA="-o Port=2201 -i $work/key" bash "$deploy/NODE-RPI4-01/install-mobile-api.sh" root@127.0.0.1 > "$work/api.log" 2>&1 || { cat "$work/api.log"; exit 1; }
check "mobile-api.json: broker 127.0.0.1, base URL = Pi4 address" "on hm-fake-rpi4 'grep -q \"\\\"host\\\": \\\"127.0.0.1\\\"\" /data/share/homemind/config/mobile-api.json && grep -q https://127.0.0.1:8000 /data/share/homemind/config/mobile-api.json'"
check "token dir network 0700" "on hm-fake-rpi4 'test \"\$(stat -c %U:%a /data/share/homemind/mobile-api)\" = network:700'"
check "Mobile API password notice + pairing hint printed" "grep -q 'MISSING' '$work/api.log' && grep -q 'print-pairing-qr' '$work/api.log'"
# core 0.6.5 already ships the phone-list ACL as its template, so a fresh install says "already current"; an older
# template on the device (0.6.3/0.6.4, the real Pi4 before 2026-10-25) is replaced and the broker reloaded.
check "broker ACL has the phone list topics, network 0640 (replaced+reloaded or already current)" "on hm-fake-rpi4 'grep -q \"topic read pfn/v1/mobile/phones/state\" /data/share/homemind/config/mosquitto.acl && test \"\$(stat -c %U:%a /data/share/homemind/config/mosquitto.acl)\" = network:640' && { grep -q 'ACL already current' '$work/api.log' || { on hm-fake-rpi4 'ls /data/share/homemind/backup/mosquitto.acl-* >/dev/null && grep -q \"kill -s HUP homemind-mqtt\" /var/log/fake-systemctl.log'; }; }"
docker cp "$(host_path "$(ls "$deploy"/NODE-RPI4-01/payload/homemind-rpi4-mobile-api_*_aarch64.ipk)")" hm-fake-rpi4:/tmp/api.ipk
check "Mobile API IPK does not ship libmosquitto (core owns it)" "on hm-fake-rpi4 'mkdir -p /tmp/apix && cd /tmp/apix && ar x /tmp/api.ipk && ! tar -tzf data.tar.gz | grep -q libmosquitto && tar -tzf data.tar.gz | grep -q bin/homemind-mobile-api'"

for node in 01 02; do
  name="hm-fake-rpi5-$node"; port="$((2210 + 10#$node))"
  echo "== NODE-RPI5-$node"
  start_device "$name" "$port"
  HM_SSH_EXTRA="-o Port=$port -i $work/key" bash "$deploy/NODE-RPI5-$node/install.sh" root@127.0.0.1 --broker-host 10.0.0.2 \
    > "$work/rpi5-$node.log" 2>&1 || { cat "$work/rpi5-$node.log"; exit 1; }
  config="rpi5-$node.json"
  check "broker host/port set in $config" "on $name 'grep -q \"\\\"host\\\": \\\"10.0.0.2\\\"\" /data/share/homemind/config/$config && grep -q \"\\\"port\\\": 1884\" /data/share/homemind/config/$config'"
  check "no placeholder left" "! on $name 'grep -rq BROKER_HOST /data/share/homemind/config/'"
  check "models copied with matching hash" "on $name 'cd /data/share/homemind/models && ls *.gguf *.bin 2>/dev/null | head -1 | grep -q .'"
  check "missing password notice printed" "grep -q 'MISSING. From this laptop' '$work/rpi5-$node.log'"
  check "units installed in /etc" "on $name 'ls /etc/systemd/system/homemind-*.service | wc -l | grep -qv ^0'"
  check "bin 0755, units/config 0644" "on $name 'test \"\$(stat -c %a /data/share/homemind/bin/\$(ls /data/share/homemind/bin | head -1))\" = 755 && test \"\$(stat -c %a /data/share/usr/lib/systemd/system/\$(ls /data/share/usr/lib/systemd/system | grep service | head -1))\" = 644'"
  check "secrets dir root:network 0750" "on $name 'test \"\$(stat -c %U:%G:%a /data/share/homemind/secrets)\" = root:network:750'"
  # second run with a password file: no notice, idempotent
  on "$name" 'umask 077; printf "%s\n" "$(head -c 12 /dev/urandom | od -An -tx1 | tr -d " \n")" > /data/share/homemind/secrets/mqtt-password; chgrp network /data/share/homemind/secrets/mqtt-password; chmod 640 /data/share/homemind/secrets/mqtt-password'
  HM_SSH_EXTRA="-o Port=$port -i $work/key" bash "$deploy/NODE-RPI5-$node/install.sh" root@127.0.0.1 --broker-host 10.0.0.2 \
    > "$work/rpi5-$node-2.log" 2>&1 || { cat "$work/rpi5-$node-2.log"; exit 1; }
  check "rerun: models up to date, no password notice" "grep -q 'up to date' '$work/rpi5-$node-2.log' && ! grep -q 'MISSING' '$work/rpi5-$node-2.log'"
done
echo "== addresses from deploy/site.env (no arguments)"
if [[ ! -e "$deploy/site.env" ]]; then
  printf 'SSH_USER=root
RPI4_01_HOST=10.0.0.3
RPI5_02_HOST=127.0.0.1
BROKER_PORT=1884
' > "$deploy/site.env"
  on hm-fake-rpi5-02 'sed -i "s/\"host\": \"10.0.0.2\"/\"host\": \"BROKER_HOST\"/" /data/share/homemind/config/rpi5-02.json'
  HM_SSH_EXTRA="-o Port=2212 -i $work/key" bash "$deploy/NODE-RPI5-02/install.sh" > "$work/site.log" 2>&1 || { rm -f "$deploy/site.env"; cat "$work/site.log"; exit 1; }
  rm -f "$deploy/site.env"
  check "site.env addresses used" "on hm-fake-rpi5-02 'grep -q \"\\\"host\\\": \\\"10.0.0.3\\\"\" /data/share/homemind/config/rpi5-02.json'"
else
  echo "  SKIP (a real deploy/site.env exists; not touched)"
fi
check "RPI5-01 got the UI (ui.env host set)" "on hm-fake-rpi5-01 'grep -qx HOMEMIND_PI4_HOST=10.0.0.2 /data/share/homemind/config/ui.env && test -x /data/share/homemind/ui/homemind-ui-launch.sh'"
check "RPI5-02 vision stays off (no vision config)" "! on hm-fake-rpi5-02 'test -f /data/share/homemind/config/rpi5-02-vision.json'"
echo "== tampered payload is refused"
cp -r "$deploy/NODE-RPI5-02/payload" "$work/tampered"; printf x >> "$work/tampered/$(ls "$work/tampered" | grep ipk)"
check "hash mismatch stops before ssh" "! HM_SSH_EXTRA='-o Port=2202 -i $work/key' bash '$deploy/common/rpi5-node.sh' 02 '$work/tampered' root@127.0.0.1 --broker-host 10.0.0.2 > /dev/null 2>&1"
# whoever changes a file can also rewrite SHA256SUMS: only the release signature catches that
(cd "$work/tampered" && find . -type f ! -name 'SHA256SUMS*' | sed 's|^\./||' | sort | xargs -d '\n' sha256sum > SHA256SUMS)
check "rewritten hash list fails the release signature" "{ HM_SSH_EXTRA='-o Port=2202 -i $work/key' bash '$deploy/common/rpi5-node.sh' 02 '$work/tampered' root@127.0.0.1 --broker-host 10.0.0.2 2>&1 || true; } | grep -q 'signature INVALID'"
rm -f "$work/tampered/SHA256SUMS.sig"
check "unsigned payload is refused" "! HM_SSH_EXTRA='-o Port=2202 -i $work/key' bash '$deploy/common/rpi5-node.sh' 02 '$work/tampered' root@127.0.0.1 --broker-host 10.0.0.2 > /dev/null 2>&1"
check "every collected payload is signed with the committed release key" "(. '$deploy/common/payload.sh'; for d in NODE-RPI4-01 NODE-RPI5-01 NODE-RPI5-02; do payload_verify '$deploy/'\$d/payload > /dev/null || exit 1; done)"
[[ $fails == 0 ]] && echo "DEPLOY_DRYRUN_PASS (fake devices, PC simulation)" || { echo "DEPLOY_DRYRUN_FAIL ($fails)"; exit 1; }
