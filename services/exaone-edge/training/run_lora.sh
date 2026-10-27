#!/usr/bin/env bash
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/exaone-edge"
venv_dir="${HOME}/venvs/homemind-exaone"
mode="${1:-smoke}"

source "$venv_dir/bin/activate"
export HF_HOME="/mnt/c/Users/alexi/Desktop/Workspace/ESW/.cache/huggingface"
export PYTHONUNBUFFERED=1
cd "$service_dir"

if [[ "$mode" == "full" ]]; then
  python training/train_lora.py
else
  python training/train_lora.py --smoke
fi
