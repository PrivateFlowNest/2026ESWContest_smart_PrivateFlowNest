#!/usr/bin/env bash
# LoRA v3: same LoRA/optimizer hyper-parameters as v1 (r=16, alpha=32, dropout=0.05,
# lr=2e-4, 3 epochs, effective batch 16). Prompts now include the client's few-shot
# turns (~2,050 tokens), so max_length is raised and the per-device batch is split
# 1 x 16 accumulation (2 x 8 spilled past 24 GB on WSL) without changing the effective batch.
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/exaone-edge"
venv_dir="${HOME}/venvs/homemind-exaone"
mode="${1:-full}"
shift || true

source "$venv_dir/bin/activate"
export HF_HOME="/mnt/c/Users/alexi/Desktop/Workspace/ESW/.cache/huggingface"
export HF_HUB_OFFLINE=1
export PYTHONUNBUFFERED=1
cd "$service_dir"

args=(
  --data-dir "$service_dir/data/finetune-v3"
  --output-dir "$service_dir/artifacts/exaone-1.2b-lora-v3"
  --max-length 2304
  --batch-size 1
  --eval-batch-size 1
  --gradient-accumulation 16
)
if [[ "$mode" == "smoke" ]]; then
  args+=(--smoke)
fi
# Extra arguments override the defaults above (argparse keeps the last value).
python training/train_lora.py "${args[@]}" "$@"
