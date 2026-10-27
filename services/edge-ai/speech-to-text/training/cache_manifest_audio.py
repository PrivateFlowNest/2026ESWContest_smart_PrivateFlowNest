"""Materialize selected utterances as a local 16 kHz mono training cache."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

import numpy as np
import soundfile as sf
import torch
import torch.nn.functional as torch_functional


def linux_path(value: str) -> Path:
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", value)
    if match:
        return Path(f"/mnt/{match.group(1).lower()}/{match.group(2).replace(chr(92), '/')}")
    return Path(value)


def windows_path(path: Path) -> str:
    resolved = path.resolve()
    parts = resolved.parts
    if len(parts) >= 4 and parts[1] == "mnt" and len(parts[2]) == 1:
        return f"{parts[2].upper()}:\\" + "\\".join(parts[3:])
    return str(resolved)


def decode_row(row: dict[str, object]) -> np.ndarray:
    source = linux_path(str(row["audio_path"]))
    with sf.SoundFile(source) as handle:
        sample_rate = handle.samplerate
        start_frame = max(0, round(float(row.get("start_seconds", 0.0)) * sample_rate))
        handle.seek(start_frame)
        end = row.get("end_seconds")
        frames = -1 if end is None else max(1, round(float(end) * sample_rate) - start_frame)
        samples = handle.read(frames, dtype="float32", always_2d=False)
    if samples.ndim == 2:
        samples = samples.mean(axis=1)
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
    if samples.size == 0 or not np.isfinite(samples).all():
        raise ValueError("decoded audio is empty or non-finite")
    return samples


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    rows = [json.loads(line) for line in args.input.read_text(encoding="utf-8").splitlines() if line]
    cached_by_id: dict[str, str] = {}
    output_rows: list[dict[str, object]] = []
    source_counts: Counter[str] = Counter()
    cache_hits = 0
    total_samples = 0

    for index, row in enumerate(rows, 1):
        row_id = str(row["id"])
        cached_path = cached_by_id.get(row_id)
        if cached_path is None:
            digest = hashlib.sha256(row_id.encode("utf-8")).hexdigest()
            destination = args.cache_dir / digest[:2] / f"{digest}.wav"
            destination.parent.mkdir(parents=True, exist_ok=True)
            valid_existing = False
            if destination.is_file():
                try:
                    info = sf.info(destination)
                    valid_existing = info.samplerate == 16_000 and info.channels == 1 and info.frames > 0
                except sf.LibsndfileError:
                    valid_existing = False
            if valid_existing:
                cache_hits += 1
            else:
                samples = decode_row(row)
                temporary = destination.with_suffix(".tmp.wav")
                sf.write(temporary, samples, 16_000, subtype="PCM_16")
                temporary.replace(destination)
                total_samples += len(samples)
            cached_path = windows_path(destination)
            cached_by_id[row_id] = cached_path
        cached_row = dict(row)
        cached_row["audio_path"] = cached_path
        cached_row.pop("start_seconds", None)
        cached_row.pop("end_seconds", None)
        cached_row["audio_cache"] = "LOCAL_16KHZ_MONO_PCM16"
        output_rows.append(cached_row)
        source_counts[str(row.get("source", "unknown"))] += 1
        if index % 500 == 0:
            print(f"cached {index:,}/{len(rows):,}", flush=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as handle:
        for row in output_rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
    summary = {
        "input_rows": len(rows),
        "output_rows": len(output_rows),
        "unique_cached_audio": len(cached_by_id),
        "existing_cache_hits": cache_hits,
        "new_audio_hours": total_samples / 16_000 / 3_600,
        "source_counts": dict(source_counts),
        "cache_format": "16 kHz mono PCM16",
        "privacy_scope": "LOCAL_TRAINING_ONLY",
        "raw_transcripts_in_summary": False,
        "source_audio_paths_in_summary": False,
    }
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
