#!/usr/bin/env bash
# Enable the HomeMind services of an already installed device for boot. The 0.1.0 packages' postinst ran
# `systemctl enable` after remounting / read-only, so it failed silently (real Pi4, 2026-10-23: the Mobile API did not
# start after a reboot). The installers do this themselves now; this is for devices installed before.
#   bash deploy/enable-units.sh rpi4       homemind-mobile-api
#   bash deploy/enable-units.sh rpi5-01    homemind-exaone homemind-llm homemind-ui
#   bash deploy/enable-units.sh rpi5-02    homemind-whisper homemind-perception homemind-vision (vision stays off
#                                          until its config exists)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[[ -f "$here/site.env" ]] || { echo "no deploy/site.env (addresses)" >&2; exit 2; }
set -a; . "$here/site.env"; set +a
. "$here/common/payload.sh"
known="$here/.known_hosts"; strict=accept-new
case "${1:-}" in
  rpi4)    host="${RPI4_01_HOST:-}"; units="homemind-core homemind-mqtt homemind-mobile-api"
           known="$here/../team/homemind-core-ui-2026-10-20/.pi4_known_hosts"; strict=yes ;;
  rpi5-01) host="${RPI5_01_HOST:-}"; units="homemind-cpu-limit homemind-exaone homemind-llm homemind-ui homemind-clock-sync.service homemind-clock-sync.timer" ;;
  rpi5-02) host="${RPI5_02_HOST:-}"; units="homemind-whisper homemind-perception homemind-vision homemind-clock-sync.service homemind-clock-sync.timer" ;;
  *) sed -n '5,8p' "$0" >&2; exit 2 ;;
esac
opts=(-o UserKnownHostsFile="$known" -o StrictHostKeyChecking="$strict")
while read -r o; do opts+=("$o"); done < <(hm_ssh_shared_opts)
echo "== enable for boot on $host: $units"
ssh "${opts[@]}" "${SSH_USER:-root}@$host" "$(hm_enable_cmd $units)"
ssh "${opts[@]}" "${SSH_USER:-root}@$host" "for u in $units; do systemctl start \$u 2>/dev/null || true; done; sleep 4; \
  for u in $units; do printf '  %s now: ' \$u; systemctl is-active \$u || true; done"
