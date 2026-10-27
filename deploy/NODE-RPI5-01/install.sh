#!/usr/bin/env bash
# Install NODE-RPI5-01 from payload/ (bash deploy/collect.sh NODE-RPI5-01). RUN ONLY AFTER THE USER APPROVES.
#   bash deploy/NODE-RPI5-01/install.sh root@<rpi5-01-ip> --broker-host <rpi4-ip> [--broker-port 1884]
#   bash deploy/NODE-RPI5-01/install.sh            (addresses from deploy/site.env)
set -euo pipefail
# Optional site values (deploy/site.env, see site.env.example): addresses only, used when no arguments are given.
site="$(cd "$(dirname "$0")/.." && pwd)/site.env"
if [[ -f "$site" ]]; then set -a; . "$site"; set +a; fi
here="$(cd "$(dirname "$0")" && pwd)"
if [[ $# -eq 0 && -n "${RPI5_01_HOST:-}" && -n "${RPI4_01_HOST:-}" ]]; then
  set -- "${SSH_USER:-root}@$RPI5_01_HOST" --broker-host "$RPI4_01_HOST" --broker-port "${BROKER_PORT:-1884}"
fi
exec bash "$here/../common/rpi5-node.sh" 01 "$here/payload" "$@"
