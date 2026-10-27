"""Build a conservative vocabulary-focused follow-up from the audited v3 cache."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import random


FOCUS_WEIGHTS = {
    "front_door": 6,
    "light": 2,
    "bathroom": 2,
    "meeting": 2,
    "ventilation": 2,
}


def read_rows(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--cached", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260904)
    args = parser.parse_args()

    base_rows = read_rows(args.base)
    supplemental = [
        row
        for row in read_rows(args.cached)
        if row.get("source") != "AIHUB-71405-TRAINING-HOME-NEAR"
    ]
    focused: list[dict[str, object]] = []
    source_counts: Counter[str] = Counter()
    keyword_weighted_counts: Counter[str] = Counter()
    for row in supplemental:
        groups = [str(group) for group in row.get("keyword_groups", [])]
        weights = [FOCUS_WEIGHTS[group] for group in groups if group in FOCUS_WEIGHTS]
        if not weights:
            continue
        repeat = max(weights)
        focused.extend(dict(row) for _ in range(repeat))
        source_counts[str(row.get("source", "unknown"))] += repeat
        for group in groups:
            if group in FOCUS_WEIGHTS:
                keyword_weighted_counts[group] += repeat

    combined = base_rows + focused
    random.Random(args.seed).shuffle(combined)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as handle:
        for row in combined:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
    summary = {
        "output_rows": len(combined),
        "base_command_rows": len(base_rows),
        "focused_supplement_rows": len(focused),
        "focus_weights": FOCUS_WEIGHTS,
        "weighted_keyword_counts": dict(keyword_weighted_counts),
        "weighted_source_counts": dict(source_counts),
        "validation_rows_included": 0,
        "raw_transcripts_in_summary": False,
    }
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
