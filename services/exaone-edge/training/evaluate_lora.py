"""Evaluate an EXAONE LoRA adapter on the untouched JSONL test split."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import time
from typing import Any

import torch
from peft import PeftModel
from transformers import AutoModelForCausalLM, AutoTokenizer

from homemind_exaone.validation import ProposalValidationError, validate_proposal


SERVICE_ROOT = Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="LGAI-EXAONE/EXAONE-4.0-1.2B")
    parser.add_argument(
        "--adapter",
        type=Path,
        default=SERVICE_ROOT / "artifacts" / "exaone-1.2b-lora-v1" / "final-adapter",
    )
    parser.add_argument(
        "--test-file",
        type=Path,
        default=SERVICE_ROOT / "data" / "finetune-v1" / "test.jsonl",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=SERVICE_ROOT / "artifacts" / "exaone-1.2b-lora-v1-test-report.json",
    )
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--max-new-tokens", type=int, default=160)
    parser.add_argument("--limit", type=int, default=None)
    return parser.parse_args()


def read_rows(path: Path, limit: int | None) -> list[dict[str, Any]]:
    rows = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    return rows[:limit] if limit else rows


def meaning_errors(actual: dict[str, Any], expected: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for key in ("intent", "device", "action", "parameters", "ambiguous"):
        if actual.get(key) != expected.get(key):
            errors.append(key)
    if sorted(actual.get("safety_flags", [])) != sorted(expected.get("safety_flags", [])):
        errors.append("safety_flags")
    expected_question = expected.get("clarification_question")
    actual_question = actual.get("clarification_question")
    if (expected_question is None) != (actual_question is None):
        errors.append("clarification_question_presence")
    return errors


def main() -> None:
    args = parse_args()
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU is required for adapter evaluation")

    rows = read_rows(args.test_file, args.limit)
    tokenizer = AutoTokenizer.from_pretrained(args.model, use_fast=True)
    tokenizer.padding_side = "left"
    if tokenizer.pad_token_id is None:
        tokenizer.pad_token = tokenizer.eos_token

    base_model = AutoModelForCausalLM.from_pretrained(
        args.model,
        dtype=torch.bfloat16,
        attn_implementation="sdpa",
    ).to("cuda")
    model = PeftModel.from_pretrained(base_model, str(args.adapter)).eval()

    results: list[dict[str, Any]] = []
    started = time.perf_counter()
    for start in range(0, len(rows), args.batch_size):
        batch_rows = rows[start : start + args.batch_size]
        prompts = [
            tokenizer.apply_chat_template(
                row["prompt"], tokenize=False, add_generation_prompt=True
            )
            for row in batch_rows
        ]
        encoded = tokenizer(
            prompts,
            return_tensors="pt",
            padding=True,
            add_special_tokens=False,
        ).to("cuda")
        input_width = encoded["input_ids"].shape[1]
        with torch.inference_mode():
            generated = model.generate(
                **encoded,
                max_new_tokens=args.max_new_tokens,
                do_sample=False,
                use_cache=True,
                pad_token_id=tokenizer.pad_token_id,
                eos_token_id=tokenizer.eos_token_id,
            )
        outputs = tokenizer.batch_decode(
            generated[:, input_width:], skip_special_tokens=True
        )
        for row, raw in zip(batch_rows, outputs, strict=True):
            parse_error = None
            contract_error = None
            proposal = None
            errors = ["json"]
            try:
                parsed = json.loads(raw.strip())
                if not isinstance(parsed, dict):
                    raise TypeError("top-level JSON value is not an object")
                proposal = parsed
                errors = meaning_errors(parsed, row["expected"])
                try:
                    validate_proposal(parsed)
                except ProposalValidationError as exc:
                    contract_error = str(exc)
            except (json.JSONDecodeError, TypeError) as exc:
                parse_error = str(exc)
            results.append(
                {
                    "id": row["id"],
                    "category": row["category"],
                    "raw": raw,
                    "proposal": proposal,
                    "json_valid": parse_error is None,
                    "contract_valid": parse_error is None and contract_error is None,
                    "meaning_match": parse_error is None and not errors,
                    "meaning_errors": errors,
                    "parse_error": parse_error,
                    "contract_error": contract_error,
                }
            )
        completed = min(start + len(batch_rows), len(rows))
        elapsed = time.perf_counter() - started
        print(
            f"[{completed}/{len(rows)}] {completed / len(rows):.1%} "
            f"elapsed={elapsed:.1f}s",
            flush=True,
        )

    elapsed = time.perf_counter() - started
    total = len(results)
    json_count = sum(item["json_valid"] for item in results)
    contract_count = sum(item["contract_valid"] for item in results)
    meaning_count = sum(item["meaning_match"] for item in results)
    report = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "base_model": args.model,
        "adapter": str(args.adapter),
        "test_file": str(args.test_file),
        "total": total,
        "json_valid_count": json_count,
        "json_valid_rate": json_count / total,
        "contract_valid_count": contract_count,
        "contract_valid_rate": contract_count / total,
        "meaning_match_count": meaning_count,
        "meaning_match_rate": meaning_count / total,
        "elapsed_seconds": elapsed,
        "samples_per_second": total / elapsed,
        "results": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({key: value for key, value in report.items() if key != "results"}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
