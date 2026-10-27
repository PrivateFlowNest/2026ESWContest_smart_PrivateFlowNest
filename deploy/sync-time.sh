#!/usr/bin/env bash
# The Pis have no battery clock and the router may have no internet: after every power-on, give each Pi this
# laptop's time (UTC). Pi4 found on 2026-10-22 at "Sep 1" after boot, 52 days behind.
#   bash deploy/sync-time.sh          (addresses from deploy/site.env; unreachable Pis are skipped)
# A Pi that was more than a minute off gets its HomeMind services restarted so records carry the right time.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/common/payload.sh"
[[ -f "$here/site.env" ]] || { echo "no deploy/site.env (addresses)" >&2; exit 2; }
set -a; . "$here/site.env"; set +a
shared=()
while read -r o; do shared+=("$o"); done < <(hm_ssh_shared_opts)

sync_one() {
  local label=$1 host=$2 services=$3 known=$4 strict=$5 before after drift
  [[ -n "$host" ]] || { echo "  $label: no address in site.env"; return; }
  local opts=(-o ConnectTimeout=5 -o UserKnownHostsFile="$known" -o StrictHostKeyChecking="$strict" "${shared[@]}")
  if ! before="$(ssh "${opts[@]}" "${SSH_USER:-root}@$host" 'date -u +%s' 2>/dev/null)"; then
    echo "  $label ($host): not reachable, skipped"
    return
  fi
  ssh "${opts[@]}" "${SSH_USER:-root}@$host" "date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')' >/dev/null" 2>/dev/null
  after="$(ssh "${opts[@]}" "${SSH_USER:-root}@$host" 'date -u +%s' 2>/dev/null)"
  drift=$(( $(date -u +%s) - before ))
  echo "  $label ($host): was ${drift#-} s $([[ $drift -ge 0 ]] && echo behind || echo ahead), now $(( $(date -u +%s) - after )) s off"
  if (( drift > 60 || drift < -60 )); then
    ssh "${opts[@]}" "${SSH_USER:-root}@$host" "systemctl restart $services 2>/dev/null || true"
    echo "  $label: restarted $services"
  fi
}

echo "== laptop UTC $(date -u '+%Y-%m-%d %H:%M:%S')"
sync_one NODE-RPI4-01 "${RPI4_01_HOST:-}" homemind-core \
  "$here/../team/homemind-core-ui-2026-10-20/.pi4_known_hosts" yes
sync_one NODE-RPI5-01 "${RPI5_01_HOST:-}" "homemind-llm homemind-ui" "$here/.known_hosts" accept-new
sync_one NODE-RPI5-02 "${RPI5_02_HOST:-}" "homemind-perception homemind-vision" "$here/.known_hosts" accept-new
