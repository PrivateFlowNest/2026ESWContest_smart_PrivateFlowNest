"""Merge the trained LoRA adapter into the original EXAONE checkpoint."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import torch
from peft import PeftModel
from transformers import AutoModelForCausalLM, AutoTokenizer


SERVICE_ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="LGAI-EXAONE/EXAONE-4.0-1.2B")
    parser.add_argument(
        "--adapter",
        type=Path,
        default=SERVICE_ROOT / "artifacts" / "exaone-1.2b-lora-v1" / "final-adapter",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=SERVICE_ROOT / "artifacts" / "exaone-1.2b-hf-merged-v1",
    )
    args = parser.parse_args()

    tokenizer = AutoTokenizer.from_pretrained(args.model, use_fast=True)
    base_model = AutoModelForCausalLM.from_pretrained(
        args.model,
        dtype=torch.bfloat16,
        low_cpu_mem_usage=True,
    )
    merged = PeftModel.from_pretrained(base_model, str(args.adapter)).merge_and_unload()
    args.output.mkdir(parents=True, exist_ok=True)
    merged.save_pretrained(
        args.output,
        safe_serialization=True,
        max_shard_size="2GB",
    )
    tokenizer.save_pretrained(args.output)
    summary = {
        "base_model": args.model,
        "adapter": str(args.adapter),
        "output": str(args.output),
        "dtype": str(merged.dtype),
    }
    (args.output / "merge-manifest.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
