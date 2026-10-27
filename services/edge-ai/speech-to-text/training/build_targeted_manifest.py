"""Build a privacy-safe, weighted HomeMind follow-up manifest from Training only."""

from __future__ import annotations

import argparse
import json
import random
from collections import Counter
from pathlib import Path

from homemind_stt.evaluation import HOMEMIND_KEYWORD_GROUPS
from homemind_stt.metrics import normalize_korean


WEIGHTS = {
    "light": 4,
    "front_door": 4,
    "bathroom": 4,
    "meeting": 4,
    "tv": 3,
    "aircon": 2,
    "sound": 2,
    "ventilation": 2,
    "washer": 1,
    "window": 1,
    "notification": 1,
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260904)
    args = parser.parse_args()

    weighted: list[dict[str, object]] = []
    source_counts: Counter[str] = Counter()
    weighted_counts: Counter[str] = Counter()
    for line in args.input.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        normalized = normalize_korean(str(row["reference"]))
        matched = [
            name
            for name, aliases in HOMEMIND_KEYWORD_GROUPS.items()
            if any(normalize_korean(alias) in normalized for alias in aliases)
        ]
        if not matched:
            continue
        repeat = max(WEIGHTS[name] for name in matched)
        for name in matched:
            source_counts[name] += 1
            weighted_counts[name] += repeat
        weighted.extend(dict(row) for _ in range(repeat))

    random.Random(args.seed).shuffle(weighted)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as handle:
        for row in weighted:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
    print(json.dumps({
        "output": str(args.output),
        "source_rows": sum(1 for _ in args.input.open(encoding="utf-8")),
        "targeted_rows": len(weighted),
        "source_keyword_counts": dict(source_counts),
        "weighted_keyword_counts": dict(weighted_counts),
        "validation_rows_included": 0,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
