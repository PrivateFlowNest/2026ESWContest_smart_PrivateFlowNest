"""Combine and deterministically shuffle local JSONL training manifests."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import random


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260904)
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    input_counts: dict[str, int] = {}
    source_counts: Counter[str] = Counter()
    for path in args.input:
        count = 0
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            row = json.loads(line)
            rows.append(row)
            source_counts[str(row.get("source", "unknown"))] += 1
            count += 1
        input_counts[str(path)] = count

    random.Random(args.seed).shuffle(rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    summary = {
        "output": str(args.output),
        "rows": len(rows),
        "input_counts": input_counts,
        "source_counts": dict(source_counts),
        "validation_rows_included": 0,
        "raw_transcripts_in_summary": False,
    }
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
