#!/usr/bin/env bash
# Separate WSL venv for the ExecuTorch trial (does not touch ~/venvs/homemind-exaone).
# Pins = optimum-executorch "stable" stack (install_dev.py @ 44fa1d333f) + xgrammar for the shim.
set -euo pipefail
VENV="${HOME}/venvs/homemind-executorch"
uv venv --python 3.12 "${VENV}"
source "${VENV}/bin/activate"
uv pip install --index-url https://download.pytorch.org/whl/cpu --extra-index-url https://pypi.org/simple \
  --index-strategy unsafe-best-match "torch==2.12.0+cpu" "torchao==0.17.0" "torchvision==0.27.0+cpu"
uv pip install --index-url https://pypi.org/simple --extra-index-url https://download.pytorch.org/whl/cpu \
  --index-strategy unsafe-best-match "executorch==1.3.1" "torch==2.12.0+cpu" "torchao==0.17.0+cpu" \
  "transformers==5.0.0rc1" "optimum~=2.0.0" \
  "git+https://github.com/huggingface/optimum-executorch@44fa1d333f7fe0a4c120d894b8d55b14928af244"
# xgrammar without deps so it cannot replace the pinned CPU torch
uv pip install --no-deps "xgrammar==0.2.8"
uv pip install "apache-tvm-ffi>=0.1.11" pydantic jinja2
