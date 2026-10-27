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
  --model-name artifacts/training/whisper-small-homemind-lora-v1/merged \
  --train-manifest artifacts/manifests/aihub-71405-home-near-training-homemind-weighted-v2.jsonl \
  --validation-manifest artifacts/manifests/aihub-71405-home-near-homemind-N-100.jsonl \
  --output-dir artifacts/training/whisper-small-homemind-lora-v2 \
  --epochs 2 \
  --learning-rate 3e-5
