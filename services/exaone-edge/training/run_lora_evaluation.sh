#!/usr/bin/env bash
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/exaone-edge"
venv_dir="${HOME}/venvs/homemind-exaone"

source "$venv_dir/bin/activate"
export HF_HOME="/mnt/c/Users/alexi/Desktop/Workspace/ESW/.cache/huggingface"
export PYTHONUNBUFFERED=1
export PYTHONPATH="$service_dir/src${PYTHONPATH:+:$PYTHONPATH}"
cd "$service_dir"

python training/evaluate_lora.py "$@"
