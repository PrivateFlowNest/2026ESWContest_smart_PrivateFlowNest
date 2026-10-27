"""Remove unreadable or out-of-range audio rows from a local training manifest."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import numpy as np
from pathlib import Path
import re
from typing import Any

import soundfile as sf


def linux_path(value: str) -> Path:
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", value)
    if match:
        return Path(f"/mnt/{match.group(1).lower()}/{match.group(2).replace(chr(92), '/')}")
    return Path(value)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    rows = [json.loads(line) for line in args.input.read_text(encoding="utf-8").splitlines() if line]
    path_status: dict[str, tuple[bool, int, int]] = {}
    invalid_reasons: Counter[str] = Counter()
    source_removed: Counter[str] = Counter()
    clean: list[dict[str, Any]] = []

    for index, row in enumerate(rows, 1):
        original_path = str(row["audio_path"])
        if original_path not in path_status:
            try:
                with sf.SoundFile(linux_path(original_path)) as handle:
                    path_status[original_path] = (True, handle.frames, handle.samplerate)
            except (OSError, RuntimeError, sf.LibsndfileError):
                path_status[original_path] = (False, 0, 0)
        readable, frame_count, sample_rate = path_status[original_path]
        reason = ""
        if not readable:
            reason = "unreadable_audio"
        elif sample_rate <= 0 or frame_count <= 0:
            reason = "empty_audio"
        else:
            start = float(row.get("start_seconds", 0.0))
            end = float(row.get("end_seconds", frame_count / sample_rate))
            if start < 0 or end <= start or round(end * sample_rate) > frame_count + sample_rate:
                reason = "segment_out_of_range"
            else:
                try:
                    with sf.SoundFile(linux_path(original_path)) as handle:
                        start_frame = max(0, round(start * sample_rate))
                        handle.seek(start_frame)
                        requested = max(1, round(end * sample_rate) - start_frame)
                        samples = handle.read(requested, dtype="float32", always_2d=False)
                    if samples.size == 0 or not np.isfinite(samples).all():
                        reason = "invalid_decoded_samples"
                except (OSError, RuntimeError, sf.LibsndfileError):
                    reason = "decode_failure"
        if reason:
            invalid_reasons[reason] += 1
            source_removed[str(row.get("source", "unknown"))] += 1
        else:
            clean.append(row)
        if index % 1_000 == 0:
            print(f"audited {index:,}/{len(rows):,}", flush=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as handle:
        for row in clean:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
    summary = {
        "input_rows": len(rows),
        "output_rows": len(clean),
        "unique_audio_files": len(path_status),
        "removed_rows": len(rows) - len(clean),
        "invalid_reasons": dict(invalid_reasons),
        "removed_source_counts": dict(source_removed),
        "raw_transcripts_in_summary": False,
        "audio_paths_in_summary": False,
        "decoded_audio_checked": True,
    }
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
