#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_path="${HOMEMIND_WSL_VENV:-$HOME/venvs/homemind-audio}"
cache_root="${HOMEMIND_CACHE_ROOT:-$HOME/homemind-cache}"

source "$venv_path/bin/activate"
export TFHUB_CACHE_DIR="$cache_root/tfhub"
mkdir -p "$TFHUB_CACHE_DIR"
cd "$project_root"

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

# v5 activity examples use the same four-second input length as Pi 5 runtime.
# Reusing the v4 temporal cache is safe because its keys include the feature
# extractor identity, source file metadata, and exact start/end times.
exec homemind-audio train \
  --manifest data/public-balanced-v5-window4s.csv \
  --output artifacts/public-balanced-v5-window4s \
  --cache "$cache_root/public-balanced-v4-temporal.sqlite3" \
  --feature-version mean-std-max-vad-v2 \
  --tune-classifier \
  --classifier-c-values 0.03,0.1,0.3,1,3,10 \
  --batch-size 8 \
  --prefetch-workers 2 \
  --prefetch-batches 2 \
  --skip-file-check
