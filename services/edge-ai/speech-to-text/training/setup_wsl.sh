#!/usr/bin/env bash
set -euo pipefail

service_dir="/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/speech-to-text"
venv_dir="${HOME}/venvs/homemind-stt"

python3 -m venv "$venv_dir"
source "$venv_dir/bin/activate"
python -m pip install --upgrade pip wheel setuptools
python -m pip install -r "$service_dir/training/requirements-wsl.txt"
python - <<'PY'
import torch
import transformers
import peft

print("torch", torch.__version__)
print("transformers", transformers.__version__)
print("peft", peft.__version__)
print("cuda_available", torch.cuda.is_available())
if not torch.cuda.is_available():
    raise SystemExit("CUDA is required for full Whisper fine-tuning")
print("gpu", torch.cuda.get_device_name(0))
PY
