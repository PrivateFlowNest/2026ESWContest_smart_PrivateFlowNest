#!/usr/bin/env bash
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/speech-to-text"
venv_dir="${HOME}/venvs/homemind-stt"
source "$venv_dir/bin/activate"
export HF_HOME="/mnt/c/Users/alexi/Desktop/Workspace/ESW/.cache/huggingface"
export PYTHONUNBUFFERED=1
export TOKENIZERS_PARALLELISM=false
cd "$service_dir"

python training/train_whisper_lora.py \
  --model-name artifacts/training/whisper-small-homemind-lora-v2/merged \
  --train-manifest artifacts/manifests/aihub-training-homemind-v4-focused.jsonl \
  --validation-manifest artifacts/manifests/aihub-71405-home-near-homemind-N-100.jsonl \
  --output-dir artifacts/training/whisper-small-homemind-lora-v4 \
  --epochs 1 \
  --train-batch 8 \
  --eval-batch 8 \
  --gradient-accumulation 2 \
  --workers 8 \
  --learning-rate 1e-5
