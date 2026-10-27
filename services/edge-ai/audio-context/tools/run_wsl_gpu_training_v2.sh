#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_path="${HOMEMIND_WSL_VENV:-$HOME/venvs/homemind-audio}"
cache_root="${HOMEMIND_CACHE_ROOT:-$HOME/homemind-cache}"

source "$venv_path/bin/activate"
export TFHUB_CACHE_DIR="$cache_root/tfhub"
mkdir -p "$TFHUB_CACHE_DIR"
cd "$project_root"

# RTX 3090 benchmark (data/rtx3090-batch-benchmark.json) selected batch 8.
# The v1 cache is intentionally reused because the feature definition is unchanged.
exec homemind-audio train \
  --manifest data/public-balanced-v2.csv \
  --output artifacts/public-balanced-v2 \
  --cache "$cache_root/public-balanced-v1.sqlite3" \
  --batch-size 8 \
  --prefetch-workers 2 \
  --prefetch-batches 2 \
  --skip-file-check
