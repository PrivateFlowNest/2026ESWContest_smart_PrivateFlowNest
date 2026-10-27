#!/usr/bin/env bash
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/speech-to-text"
venv_dir="${HOME}/venvs/homemind-stt"
mode="${1:-full}"

source "$venv_dir/bin/activate"
export HF_HOME="/mnt/c/Users/alexi/Desktop/Workspace/ESW/.cache/huggingface"
export PYTHONUNBUFFERED=1
export TOKENIZERS_PARALLELISM=false
cd "$service_dir"

extra_args=()
output_dir="artifacts/training/whisper-small-homemind-lora-v1"
if [[ "$mode" == "smoke" ]]; then
  extra_args+=(--max-train-samples 64 --max-eval-samples 32 --epochs 1)
  output_dir="artifacts/training/whisper-small-homemind-lora-realdata-smoke-not-for-release"
fi

python training/train_whisper_lora.py \
  --train-manifest artifacts/manifests/aihub-71405-home-near-training-iot-N.jsonl \
  --validation-manifest artifacts/manifests/aihub-71405-home-near-homemind-N-100.jsonl \
  --output-dir "$output_dir" \
  "${extra_args[@]}"
