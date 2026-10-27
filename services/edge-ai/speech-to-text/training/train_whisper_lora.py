"""LoRA-tune multilingual Whisper small on original AI Hub Korean commands."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import random
import re
from typing import Any

import numpy as np
import soundfile as sf
import torch
import torch.nn.functional as torch_functional
from torch.utils.data import Dataset
from transformers import (
    Seq2SeqTrainer,
    Seq2SeqTrainingArguments,
    WhisperForConditionalGeneration,
    WhisperProcessor,
    set_seed,
)
from peft import LoraConfig, get_peft_model
from jiwer import cer


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-manifest", type=Path, required=True)
    parser.add_argument("--validation-manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--model-name", default="openai/whisper-small")
    parser.add_argument("--epochs", type=float, default=3.0)
    parser.add_argument("--train-batch", type=int, default=8)
    parser.add_argument("--eval-batch", type=int, default=8)
    parser.add_argument("--gradient-accumulation", type=int, default=2)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--max-train-samples", type=int, default=0)
    parser.add_argument("--max-eval-samples", type=int, default=0)
    parser.add_argument("--seed", type=int, default=20260903)
    return parser.parse_args()


def linux_path(value: str) -> Path:
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", value)
    if match:
        return Path(f"/mnt/{match.group(1).lower()}/{match.group(2).replace(chr(92), '/')}")
    return Path(value)


def load_rows(path: Path, *, limit: int, seed: int) -> list[dict[str, Any]]:
    rows = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    random.Random(seed).shuffle(rows)
    return rows if limit <= 0 else rows[:limit]


class ManifestDataset(Dataset):
    def __init__(self, rows: list[dict[str, Any]], processor: WhisperProcessor) -> None:
        self.rows = rows
        self.processor = processor

    def __len__(self) -> int:
        return len(self.rows)

    def __getitem__(self, index: int) -> dict[str, Any]:
        row = self.rows[index]
        audio_path = linux_path(str(row["audio_path"]))
        start_seconds = float(row.get("start_seconds", 0.0))
        end_value = row.get("end_seconds")
        try:
            with sf.SoundFile(audio_path) as audio_file:
                sample_rate = audio_file.samplerate
                start_frame = max(0, round(start_seconds * sample_rate))
                audio_file.seek(start_frame)
                frames = -1 if end_value is None else max(
                    1, round(float(end_value) * sample_rate) - start_frame
                )
                samples = audio_file.read(frames, dtype="float32", always_2d=False)
        except Exception as error:
            raise RuntimeError(f"unable to read training audio for id={row['id']}: {audio_path}") from error
        if samples.ndim == 2:
            samples = samples.mean(axis=1)
        if samples.ndim != 1:
            raise ValueError(f"expected mono-compatible audio for {row['id']}")
        if sample_rate != 16_000:
            target_length = max(1, round(len(samples) * 16_000 / sample_rate))
            samples = (
                torch_functional.interpolate(
                    torch.from_numpy(samples).view(1, 1, -1),
                    size=target_length,
                    mode="linear",
                    align_corners=False,
                )
                .view(-1)
                .numpy()
            )
            sample_rate = 16_000
        processed = self.processor.feature_extractor(
            samples, sampling_rate=sample_rate, return_attention_mask=True
        )
        labels = self.processor.tokenizer(str(row["reference"])).input_ids
        return {
            "input_features": processed.input_features[0],
            "attention_mask": processed.attention_mask[0],
            "labels": labels,
        }


@dataclass
class DataCollatorSpeechSeq2Seq:
    processor: WhisperProcessor
    decoder_start_token_id: int

    def __call__(self, features: list[dict[str, Any]]) -> dict[str, torch.Tensor]:
        batch = self.processor.feature_extractor.pad(
            [
                {
                    "input_features": item["input_features"],
                    "attention_mask": item["attention_mask"],
                }
                for item in features
            ],
            return_tensors="pt",
        )
        label_batch = self.processor.tokenizer.pad(
            [{"input_ids": item["labels"]} for item in features],
            return_tensors="pt",
        )
        labels = label_batch["input_ids"].masked_fill(
            label_batch.attention_mask.ne(1), -100
        )
        if (labels[:, 0] == self.decoder_start_token_id).all().item():
            labels = labels[:, 1:]
        batch["labels"] = labels
        return batch


def main() -> None:
    args = parse_args()
    set_seed(args.seed)
    if not torch.cuda.is_available():
        raise SystemExit("CUDA GPU is required")
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True

    processor = WhisperProcessor.from_pretrained(
        args.model_name, language="Korean", task="transcribe"
    )
    model = WhisperForConditionalGeneration.from_pretrained(
        args.model_name,
        low_cpu_mem_usage=True,
    )
    model.generation_config.language = "ko"
    model.generation_config.task = "transcribe"
    model.generation_config.forced_decoder_ids = None
    model.config.use_cache = False
    lora = LoraConfig(
        r=16,
        lora_alpha=32,
        lora_dropout=0.05,
        bias="none",
        target_modules=["q_proj", "v_proj"],
    )
    model = get_peft_model(model, lora)
    model.print_trainable_parameters()

    train_rows = load_rows(
        args.train_manifest, limit=args.max_train_samples, seed=args.seed
    )
    validation_rows = load_rows(
        args.validation_manifest, limit=args.max_eval_samples, seed=args.seed + 1
    )
    train_dataset = ManifestDataset(train_rows, processor)
    validation_dataset = ManifestDataset(validation_rows, processor)
    collator = DataCollatorSpeechSeq2Seq(
        processor, model.config.decoder_start_token_id
    )

    def compute_metrics(prediction) -> dict[str, float]:
        predicted = prediction.predictions
        if isinstance(predicted, tuple):
            predicted = predicted[0]
        labels = np.array(prediction.label_ids)
        labels[labels == -100] = processor.tokenizer.pad_token_id
        hypotheses = processor.tokenizer.batch_decode(predicted, skip_special_tokens=True)
        references = processor.tokenizer.batch_decode(labels, skip_special_tokens=True)
        return {"cer": float(cer(references, hypotheses))}

    training_args = Seq2SeqTrainingArguments(
        output_dir=str(args.output_dir),
        num_train_epochs=args.epochs,
        per_device_train_batch_size=args.train_batch,
        per_device_eval_batch_size=args.eval_batch,
        gradient_accumulation_steps=args.gradient_accumulation,
        learning_rate=args.learning_rate,
        warmup_ratio=0.05,
        lr_scheduler_type="cosine",
        fp16=True,
        tf32=True,
        # Whisper small LoRA fits a 24 GB 3090 without checkpointing. Keeping
        # it off avoids recomputation and feeds the GPU more continuously.
        gradient_checkpointing=False,
        predict_with_generate=True,
        generation_max_length=128,
        eval_strategy="epoch",
        save_strategy="epoch",
        logging_steps=20,
        load_best_model_at_end=True,
        metric_for_best_model="cer",
        greater_is_better=False,
        save_total_limit=2,
        remove_unused_columns=False,
        label_names=["labels"],
        dataloader_num_workers=args.workers,
        dataloader_pin_memory=True,
        dataloader_persistent_workers=args.workers > 0,
        dataloader_prefetch_factor=4 if args.workers > 0 else None,
        group_by_length=False,
        report_to=[],
        seed=args.seed,
    )
    trainer = Seq2SeqTrainer(
        model=model,
        args=training_args,
        train_dataset=train_dataset,
        eval_dataset=validation_dataset,
        data_collator=collator,
        compute_metrics=compute_metrics,
        processing_class=processor,
    )
    train_result = trainer.train()
    evaluation = trainer.evaluate()

    adapter_dir = args.output_dir / "adapter"
    merged_dir = args.output_dir / "merged"
    trainer.model.save_pretrained(adapter_dir)
    processor.save_pretrained(adapter_dir)
    merged = trainer.model.merge_and_unload()
    merged.save_pretrained(merged_dir, safe_serialization=True)
    processor.save_pretrained(merged_dir)

    summary = {
        "schema_version": "1.0",
        "base_model": args.model_name,
        "train_rows": len(train_rows),
        "validation_rows": len(validation_rows),
        "epochs": args.epochs,
        "effective_batch_size": (
            args.train_batch * args.gradient_accumulation
        ),
        "train_metrics": train_result.metrics,
        "validation_metrics": evaluation,
        "privacy": {
            "selected_audio_cached_locally": any(
                "audio_cache" in row for row in train_rows
            ),
            "raw_audio_exported": False,
            "transcripts_in_summary": False,
            "audio_paths_in_summary": False,
        },
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "training-summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2, default=str) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2, default=str))


if __name__ == "__main__":
    main()
