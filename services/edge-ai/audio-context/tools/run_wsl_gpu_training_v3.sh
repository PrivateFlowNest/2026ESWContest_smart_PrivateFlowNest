#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_path="${HOMEMIND_WSL_VENV:-$HOME/venvs/homemind-audio}"
cache_root="${HOMEMIND_CACHE_ROOT:-$HOME/homemind-cache}"

source "$venv_path/bin/activate"
export TFHUB_CACHE_DIR="$cache_root/tfhub"
mkdir -p "$TFHUB_CACHE_DIR"
cd "$project_root"

# Windows WDDM graphics activity can briefly report high total GPU utilization
# even when no CUDA training job exists. Block only on an actual compute process.
if pgrep -f '[h]omemind-audio train' >/dev/null 2>&1; then
  echo "Another HomeMind training process is already running; refusing a duplicate."
  exit 2
fi

if command -v nvidia-smi >/dev/null 2>&1; then
  compute_pids="$(nvidia-smi --query-compute-apps=pid --format=csv,noheader,nounits 2>/dev/null | sed '/^[[:space:]]*$/d' || true)"
  if [[ -n "$compute_pids" ]]; then
    echo "CUDA compute process already active (PID: $(echo "$compute_pids" | paste -sd, -)); refusing to start."
    exit 2
  fi
  nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu \
    --format=csv,noheader
fi

# RTX 3090 equivalence benchmark selected batch 8. The v1 cache remains valid.
exec homemind-audio train \
  --manifest data/public-balanced-v3.csv \
  --output artifacts/public-balanced-v3 \
  --cache "$cache_root/public-balanced-v1.sqlite3" \
  --batch-size 8 \
  --prefetch-workers 2 \
  --prefetch-batches 2 \
  --skip-file-check
