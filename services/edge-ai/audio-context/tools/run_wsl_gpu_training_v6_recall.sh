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

# Refit only the high-recall profile already compared on validation in the v6
# search. The independent test split remains excluded from fitting and selection.
exec homemind-audio train \
  --manifest data/public-balanced-v6-window4s.csv \
  --output artifacts/public-balanced-v6-recall-buffer \
  --cache "$cache_root/public-balanced-v4-temporal.sqlite3" \
  --feature-version mean-std-max-vad-v2 \
  --head activity \
  --tune-classifier \
  --cost-sensitive-activity \
  --activity-cost-profile recall-buffer \
  --classifier-c-values 0.03 \
  --batch-size 8 \
  --prefetch-workers 2 \
  --prefetch-batches 2 \
  --skip-file-check
