#!/usr/bin/env bash
# Merge LoRA v3 into the base EXAONE checkpoint and convert it to an f16 GGUF.
# Q4_K_M quantization is done afterwards with llama-quantize (WinGet llama.cpp build).
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/exaone-edge"
llama_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/third_party/llama.cpp"
venv_dir="${HOME}/venvs/homemind-exaone"

source "$venv_dir/bin/activate"
export HF_HOME="/mnt/c/Users/alexi/Desktop/Workspace/ESW/.cache/huggingface"
export HF_HUB_OFFLINE=1
export PYTHONUNBUFFERED=1
cd "$service_dir"

python training/merge_lora.py \
  --adapter "$service_dir/artifacts/exaone-1.2b-lora-v3/final-adapter" \
  --output "$service_dir/artifacts/exaone-1.2b-hf-merged-v3"

cd /tmp
PYTHONPATH="$llama_dir/gguf-py" python "$llama_dir/convert_hf_to_gguf.py" \
  "$service_dir/artifacts/exaone-1.2b-hf-merged-v3" \
  --outtype f16 \
  --outfile "$service_dir/artifacts/exaone-1.2b-homemind-v3-f16.gguf"
