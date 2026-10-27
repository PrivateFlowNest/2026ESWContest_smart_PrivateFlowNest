#!/usr/bin/env bash
# Plan 2 trial (system-architecture 3-2): export the adopted LoRA v3 merged EXAONE 4.0 1.2B
# checkpoint to an ExecuTorch .pte with the XNNPACK delegate, via the official
# optimum-executorch flow (torch.export of the Hugging Face Exaone4ForCausalLM + static KV cache).
#
#   bash export_pte.sh [8da4w|8da8w]        (run inside WSL; venv: ~/venvs/homemind-executorch)
#
# Versions are pinned to the optimum-executorch "stable" stack (install_dev.py at commit 44fa1d333f):
#   torch 2.12.0+cpu, torchao 0.17.0+cpu, executorch 1.3.1, transformers 5.0.0rc1, optimum 2.0.0,
#   optimum-executorch git 44fa1d333f (0.2.0.dev0). See setup_venv.sh.
#
# Output: services/exaone-edge/artifacts/executorch/<variant>/model.pte (*.pte is gitignored).
# Note: the causal_lm task of optimum-executorch ignores --qlinear_group_size; linear layers use
# group size 32 and the (tied) embedding uses per-row int8.
set -euo pipefail

VARIANT="${1:-8da4w}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE="$(cd "${HERE}/../.." && pwd)"
MODEL="${SERVICE}/artifacts/exaone-1.2b-hf-merged-v3"
OUT="${SERVICE}/artifacts/executorch/exaone-1.2b-homemind-v3-${VARIANT}"
MAX_SEQ_LEN="${MAX_SEQ_LEN:-4096}"   # same context as llama-server -c 4096; prompts are <= ~2.1k tokens

source "${HOME}/venvs/homemind-executorch/bin/activate"
mkdir -p "${OUT}"

case "${VARIANT}" in
  8da4w) QARGS=(--qlinear 8da4w --qembedding 8w) ;;
  8da8w) QARGS=(--qlinear 8da8w --qembedding 8w) ;;
  *) echo "unknown variant ${VARIANT}" >&2; exit 2 ;;
esac

python -c "import torch, torchao, transformers; from importlib.metadata import version as v; \
print('torch', torch.__version__, 'torchao', torchao.__version__, 'executorch', v('executorch'), \
'transformers', transformers.__version__, 'optimum-executorch', v('optimum-executorch'))"

/usr/bin/time -v optimum-cli export executorch \
  --model "${MODEL}" \
  --task text-generation \
  --recipe xnnpack \
  --use_custom_sdpa \
  --use_custom_kv_cache \
  "${QARGS[@]}" \
  --max_seq_len "${MAX_SEQ_LEN}" \
  --dtype float32 \
  --output_dir "${OUT}"

ls -l "${OUT}"
sha256sum "${OUT}"/*.pte
