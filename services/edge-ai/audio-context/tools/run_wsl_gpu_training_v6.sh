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
  nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu \
    --format=csv,noheader
fi

# The test groups were reserved before windowing and are never used for model
# selection. Existing v5 embeddings are reused because cache identity is based on
# source metadata and exact time range rather than the manifest split name.
exec homemind-audio train \
  --manifest data/public-balanced-v6-window4s.csv \
  --output artifacts/public-balanced-v6-cost-sensitive \
  --cache "$cache_root/public-balanced-v4-temporal.sqlite3" \
  --feature-version mean-std-max-vad-v2 \
  --head activity \
  --tune-classifier \
  --cost-sensitive-activity \
  --classifier-c-values 0.03 \
  --batch-size 8 \
  --prefetch-workers 2 \
  --prefetch-batches 2 \
  --skip-file-check
