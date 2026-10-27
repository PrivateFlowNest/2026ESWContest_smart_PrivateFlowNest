#!/usr/bin/env bash
# Give a Pi5 its fixed LAN address on the device itself, like NODE-RPI4-01 (10.10.10.2 from the core package), so a
# replacement router only needs 10.10.10.1/24 with DHCP from .100 (user decision 2026-10-23: no per-device router
# settings). RUN ONLY AFTER THE USER APPROVES.
#   bash deploy/set-static-ip.sh rpi5-01 <current address>    -> RPI5_01_HOST of site.env (10.10.10.3)
#   bash deploy/set-static-ip.sh rpi5-02 <current address>    -> RPI5_02_HOST of site.env (10.10.10.4)
# Installs homemind-static-ip.service + .timer in /data/share/usr/lib/systemd/system with their enable links there
# (Atlas keeps / read-only; the core package enables homemind-ethernet.service the same way). The timer adds the
# address again every minute in case the network manager drops it; a DHCP address, if any, stays as well.
# It also gives eth0 the board's own MAC (homemind-board-mac.sh): Atlas boots every Pi 5 with one fixed device tree,
# so both real Pi5s had eth0 88:a2:9e:3c:ce:ca and could not reach each other (2026-10-25, user approved the fix).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[[ -f "$here/site.env" ]] || { echo "no deploy/site.env (addresses)" >&2; exit 2; }
set -a; . "$here/site.env"; set +a
. "$here/common/payload.sh"
case "${1:-}" in
  rpi5-01) node=NODE-RPI5-01; fixed="${RPI5_01_HOST:-}" ;;
  rpi5-02) node=NODE-RPI5-02; fixed="${RPI5_02_HOST:-}" ;;
  *) sed -n '5,6p' "$0" >&2; exit 2 ;;
esac
current="${2:-$fixed}"
[[ "$fixed" =~ ^10\.10\.10\.[0-9]+$ ]] || { echo "$node: no 10.10.10.x address in site.env" >&2; exit 2; }
opts=(-o UserKnownHostsFile="$here/.known_hosts" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8)
while read -r o; do opts+=("$o"); done < <(hm_ssh_shared_opts)
on() { ssh "${opts[@]}" "${SSH_USER:-root}@$current" "$@"; }
U=/data/share/usr/lib/systemd/system

echo "== 1. $node now at $current"
ifname="$(on "ip -4 addr show | awk -v a='$current/' '\$1 == \"inet\" && index(\$2, a) == 1 {print \$NF; exit}'")"
ipbin="$(on 'command -v ip')"
[[ -n "$ifname" ]] || { echo "  no interface holds $current" >&2; exit 1; }
echo "  interface $ifname, $ipbin"

echo "== 2. homemind-static-ip.service + .timer -> $fixed/24 on $ifname (and the board's own MAC)"
on "set -e; mkdir -p $U; cat > $U/homemind-board-mac.sh; chmod 0755 $U/homemind-board-mac.sh" <<'EOF'
#!/bin/sh
# homemind-static-ip.service: give the interface the board's own MAC. Atlas boots every Pi 5 with one fixed device
# tree, so every Pi 5 gets eth0 88:a2:9e:3c:ce:ca and two of them cannot reach each other (real Pi5-01 and Pi5-02,
# 2026-10-25). The firmware still passes the board's MAC as macaddr= on the kernel command line. Runs at boot and
# every minute; once the MAC is right it does nothing.
#   homemind-board-mac.sh <interface> <fixed address>
dev="${1:?interface}"
fixed="${2:?fixed address}"
mac="$(sed -n 's/.*macaddr=\([0-9A-Fa-f:]\{17\}\).*/\1/p' "${HM_CMDLINE:-/proc/cmdline}" | tr 'A-F' 'a-f')"
[ -n "$mac" ] || { echo "no macaddr= on the kernel command line: MAC left as is"; exit 0; }
current="$(cat "/sys/class/net/$dev/address")"
[ "$current" = "$mac" ] && exit 0
ip link set dev "$dev" down
ip link set dev "$dev" address "$mac"
ip link set dev "$dev" up
ip address replace "$fixed/24" dev "$dev"
echo "$dev: MAC $current -> $mac (the board's own MAC from the firmware)"
# tell the router and the other hosts now instead of when their ARP entries expire
arping -U -c 2 -I "$dev" -s "$fixed" "$fixed" >/dev/null 2>&1 || true
EOF
on "set -e; mkdir -p $U/multi-user.target.wants $U/timers.target.wants; cat > $U/homemind-static-ip.service" <<EOF
[Unit]
Description=HomeMind fixed LAN address $fixed and board MAC ($node, deploy/set-static-ip.sh)
After=local-fs.target connman.service

[Service]
Type=oneshot
ExecStart=/bin/sh $U/homemind-board-mac.sh $ifname $fixed
ExecStart=$ipbin link set $ifname up
ExecStart=$ipbin address replace $fixed/24 dev $ifname

[Install]
WantedBy=multi-user.target
EOF
on "cat > $U/homemind-static-ip.timer" <<'EOF'
[Unit]
Description=HomeMind fixed LAN address: add it again if it was dropped

[Timer]
OnBootSec=20s
OnUnitActiveSec=60s
AccuracySec=5s

[Install]
WantedBy=timers.target
EOF
on "set -e; ln -sf ../homemind-static-ip.service $U/multi-user.target.wants/homemind-static-ip.service; \
  ln -sf ../homemind-static-ip.timer $U/timers.target.wants/homemind-static-ip.timer; systemctl daemon-reload"

echo "== 2b. enable for boot in /etc (same as the HomeMind packages' postinst: Atlas builds the boot set from /etc,"
echo "       units only in /data did not start after a reboot of the real Pi5-02, 2026-10-23; / is remounted rw briefly)"
on "ro=0; case \",\$(awk '\$2 == \"/\" { print \$4; exit }' /proc/mounts),\" in *,ro,*) mount -o remount,rw / && ro=1;; esac; \
  ( set -e; E=/etc/systemd/system; mkdir -p \$E/multi-user.target.wants \$E/timers.target.wants; \
    for u in homemind-static-ip.service homemind-static-ip.timer; do cat $U/\$u > \$E/\$u.new; chmod 0644 \$E/\$u.new; mv \$E/\$u.new \$E/\$u; done; \
    ln -sf \$E/homemind-static-ip.service \$E/multi-user.target.wants/homemind-static-ip.service; \
    ln -sf \$E/homemind-static-ip.timer \$E/timers.target.wants/homemind-static-ip.timer ); rc=\$?; \
  sync; if [ \$ro -eq 1 ]; then mount -o remount,ro /; fi; [ \$rc -eq 0 ] || exit \$rc; systemctl daemon-reload; \
  printf '  enabled: '; systemctl is-enabled homemind-static-ip.service homemind-static-ip.timer | tr '\n' ' '; echo; \
  printf '  / is '; awk '\$2 == \"/\" { split(\$4, o, \",\"); print o[1]; exit }' /proc/mounts"

echo "== 2c. start it (only now: a unit in /etc wins over $U, so on a second run starting before 2b ran the old copy"
echo "       from the first run, real Pi5-02 2026-10-25; a MAC change can stall this ssh for a few seconds)"
on "systemctl start homemind-static-ip.service homemind-static-ip.timer; \
  ip -4 addr show $ifname | grep inet; printf '  MAC '; cat /sys/class/net/$ifname/address; journalctl -u homemind-static-ip -n 20 --no-pager | grep 'MAC ' | tail -n 1; \
  printf '  timer '; systemctl is-active homemind-static-ip.timer"

echo "== 3. reachable at $fixed from this laptop"
if ssh "${opts[@]}" "${SSH_USER:-root}@$fixed" 'echo "  OK: $(uname -n) answers on the fixed address"'; then
  echo "done. Check once after a reboot:  ssh root@$fixed 'reboot'  then (1-2 min)  ssh root@$fixed 'ip -4 addr show $ifname'"
else
  echo "  FAIL: $fixed does not answer (the address is on the device; check the cable/router subnet)" >&2
  exit 1
fi
