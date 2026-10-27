#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
venv_path="${HOMEMIND_WSL_VENV:-$HOME/venvs/homemind-audio}"
cache_root="${HOMEMIND_CACHE_ROOT:-$HOME/homemind-cache}"

source "$venv_path/bin/activate"
export TFHUB_CACHE_DIR="$cache_root/tfhub"
mkdir -p "$TFHUB_CACHE_DIR"
cd "$project_root"

exec homemind-audio train \
  --manifest data/public-balanced-v1.csv \
  --output artifacts/public-balanced-v1 \
  --cache "$cache_root/public-balanced-v1.sqlite3" \
  --skip-file-check
