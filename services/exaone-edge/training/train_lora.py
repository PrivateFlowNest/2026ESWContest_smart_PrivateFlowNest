"""LoRA SFT for EXAONE 4.0 1.2B on a single RTX 3090."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import random
from typing import Any

import torch
from peft import LoraConfig, get_peft_model
from torch.utils.data import Dataset
from transformers import (
    AutoModelForCausalLM,
    AutoTokenizer,
    Trainer,
    TrainingArguments,
    set_seed,
)


SERVICE_ROOT = Path(__file__).resolve().parents[1]


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if not rows:
        raise ValueError(f"No records in {path}")
    return rows


class CompletionDataset(Dataset):
    def __init__(
        self,
        rows: list[dict[str, Any]],
        tokenizer: Any,
        *,
        max_length: int,
    ) -> None:
        self.items: list[dict[str, list[int]]] = []
        self.max_observed_length = 0
        for row in rows:
            prompt_ids = tokenizer.apply_chat_template(
                row["prompt"],
                tokenize=True,
                add_generation_prompt=True,
            )
            completion_text = row["completion"][0]["content"]
            completion_ids = tokenizer(
                completion_text,
                add_special_tokens=False,
            )["input_ids"]
            if tokenizer.eos_token_id is not None:
                completion_ids = [*completion_ids, tokenizer.eos_token_id]
            input_ids = [*prompt_ids, *completion_ids]
            self.max_observed_length = max(self.max_observed_length, len(input_ids))
            if len(input_ids) > max_length:
                raise ValueError(
                    f"Record {row['id']} has {len(input_ids)} tokens, above --max-length {max_length}"
                )
            self.items.append(
                {
                    "input_ids": input_ids,
                    "attention_mask": [1] * len(input_ids),
                    "labels": [-100] * len(prompt_ids) + completion_ids,
                }
            )

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, index: int) -> dict[str, list[int]]:
        return self.items[index]


@dataclass
class CompletionCollator:
    pad_token_id: int

    def __call__(self, features: list[dict[str, list[int]]]) -> dict[str, torch.Tensor]:
        longest = max(len(item["input_ids"]) for item in features)
        batch = {"input_ids": [], "attention_mask": [], "labels": []}
        for item in features:
            padding = longest - len(item["input_ids"])
            batch["input_ids"].append(item["input_ids"] + [self.pad_token_id] * padding)
            batch["attention_mask"].append(item["attention_mask"] + [0] * padding)
            batch["labels"].append(item["labels"] + [-100] * padding)
        return {name: torch.tensor(value, dtype=torch.long) for name, value in batch.items()}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="LGAI-EXAONE/EXAONE-4.0-1.2B")
    parser.add_argument("--data-dir", type=Path, default=SERVICE_ROOT / "data" / "finetune-v1")
    parser.add_argument("--output-dir", type=Path, default=SERVICE_ROOT / "artifacts" / "exaone-1.2b-lora-v1")
    parser.add_argument("--max-length", type=int, default=1024)
    parser.add_argument("--epochs", type=float, default=3.0)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--eval-batch-size", type=int, default=4)
    parser.add_argument("--gradient-accumulation", type=int, default=4)
    parser.add_argument("--learning-rate", type=float, default=2e-4)
    parser.add_argument("--lora-r", type=int, default=16)
    parser.add_argument("--lora-alpha", type=int, default=32)
    parser.add_argument("--lora-dropout", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=20260902)
    parser.add_argument("--num-workers", type=int, default=4)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--resume-from-checkpoint", default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU is required for this training command")

    set_seed(args.seed)
    tokenizer = AutoTokenizer.from_pretrained(args.model, use_fast=True)
    if tokenizer.pad_token_id is None:
        tokenizer.pad_token = tokenizer.eos_token

    train_rows = read_jsonl(args.data_dir / "train.jsonl")
    validation_rows = read_jsonl(args.data_dir / "validation.jsonl")
    if args.smoke:
        rng = random.Random(args.seed)
        train_rows = rng.sample(train_rows, min(96, len(train_rows)))
        validation_rows = rng.sample(validation_rows, min(48, len(validation_rows)))
        args.output_dir = args.output_dir.with_name(args.output_dir.name + "-smoke")

    train_dataset = CompletionDataset(train_rows, tokenizer, max_length=args.max_length)
    eval_dataset = CompletionDataset(validation_rows, tokenizer, max_length=args.max_length)
    print(
        json.dumps(
            {
                "mode": "smoke" if args.smoke else "full",
                "gpu": torch.cuda.get_device_name(0),
                "train_examples": len(train_dataset),
                "validation_examples": len(eval_dataset),
                "max_observed_tokens": max(
                    train_dataset.max_observed_length,
                    eval_dataset.max_observed_length,
                ),
                "output_dir": str(args.output_dir),
            },
            ensure_ascii=False,
            indent=2,
        ),
        flush=True,
    )

    model = AutoModelForCausalLM.from_pretrained(
        args.model,
        dtype=torch.bfloat16,
        attn_implementation="sdpa",
    )
    model.config.use_cache = False
    lora_config = LoraConfig(
        r=args.lora_r,
        lora_alpha=args.lora_alpha,
        lora_dropout=args.lora_dropout,
        bias="none",
        target_modules="all-linear",
        task_type="CAUSAL_LM",
    )
    model = get_peft_model(model, lora_config)
    model.print_trainable_parameters()

    if args.smoke:
        strategy = "steps"
        eval_steps = 3
        save_steps = 3
        max_steps = 3
        logging_steps = 1
    else:
        strategy = "epoch"
        eval_steps = None
        save_steps = 500
        max_steps = -1
        logging_steps = 5

    training_args = TrainingArguments(
        output_dir=str(args.output_dir),
        overwrite_output_dir=not bool(args.resume_from_checkpoint),
        num_train_epochs=args.epochs,
        max_steps=max_steps,
        per_device_train_batch_size=args.batch_size,
        per_device_eval_batch_size=args.eval_batch_size,
        gradient_accumulation_steps=args.gradient_accumulation,
        learning_rate=args.learning_rate,
        lr_scheduler_type="cosine",
        warmup_ratio=0.05,
        weight_decay=0.01,
        bf16=True,
        tf32=True,
        optim="adamw_torch_fused",
        eval_strategy=strategy,
        save_strategy=strategy,
        eval_steps=eval_steps,
        save_steps=save_steps,
        save_total_limit=2,
        logging_steps=logging_steps,
        logging_first_step=True,
        report_to="none",
        load_best_model_at_end=True,
        metric_for_best_model="eval_loss",
        greater_is_better=False,
        dataloader_num_workers=args.num_workers,
        dataloader_pin_memory=True,
        group_by_length=True,
        remove_unused_columns=False,
        seed=args.seed,
        data_seed=args.seed,
    )
    trainer = Trainer(
        model=model,
        args=training_args,
        train_dataset=train_dataset,
        eval_dataset=eval_dataset,
        data_collator=CompletionCollator(tokenizer.pad_token_id),
    )
    train_result = trainer.train(resume_from_checkpoint=args.resume_from_checkpoint)
    eval_result = trainer.evaluate()
    final_dir = args.output_dir / "final-adapter"
    trainer.save_model(str(final_dir))
    tokenizer.save_pretrained(str(final_dir))

    manifest = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "base_model": args.model,
        "dataset_manifest": json.loads((args.data_dir / "manifest.json").read_text(encoding="utf-8")),
        "smoke": args.smoke,
        "seed": args.seed,
        "python": platform.python_version(),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
        "gpu": torch.cuda.get_device_name(0),
        "max_length": args.max_length,
        "train_examples": len(train_dataset),
        "validation_examples": len(eval_dataset),
        "lora": {
            "r": args.lora_r,
            "alpha": args.lora_alpha,
            "dropout": args.lora_dropout,
            "target_modules": "all-linear",
        },
        "training": train_result.metrics,
        "evaluation": eval_result,
        "batching": {
            "train_batch_size": args.batch_size,
            "eval_batch_size": args.eval_batch_size,
            "gradient_accumulation": args.gradient_accumulation,
            "effective_train_batch_size": args.batch_size * args.gradient_accumulation,
            "group_by_length": True,
        },
    }
    (args.output_dir / "run-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, default=str) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(manifest, ensure_ascii=False, indent=2, default=str), flush=True)


if __name__ == "__main__":
    main()
