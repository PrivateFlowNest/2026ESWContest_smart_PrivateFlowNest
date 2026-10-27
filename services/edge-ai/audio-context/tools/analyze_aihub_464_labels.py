from __future__ import annotations

import argparse
import json
import sys
import zipfile
from collections import Counter
from pathlib import Path
from typing import Any


FIELDS = ("category", "media", "communication", "type", "domain")


def _text(value: Any) -> str:
    if value is None:
        return "(none)"
    value = str(value).strip()
    return value or "(empty)"


def _duration_seconds(document: dict[str, Any]) -> float:
    ends: list[float] = []
    for utterance in document.get("utterance", []):
        try:
            ends.append(float(utterance.get("end", 0)))
        except (TypeError, ValueError):
            continue
    return max(ends, default=0.0)


def analyze_zip(path: Path) -> dict[str, Any]:
    counters = {field: Counter() for field in FIELDS}
    speaker_counts: Counter[int] = Counter()
    environments: Counter[str] = Counter()
    meeting_count = 0
    utterance_count = 0
    duration_seconds = 0.0
    errors: list[str] = []

    with zipfile.ZipFile(path) as archive:
        json_entries = [
            entry for entry in archive.infolist() if entry.filename.lower().endswith(".json")
        ]
        uncompressed_bytes = sum(entry.file_size for entry in archive.infolist())

        for entry in json_entries:
            try:
                document = json.loads(archive.read(entry).decode("utf-8-sig"))
            except Exception as exc:  # report malformed source records without stopping the scan
                if len(errors) < 20:
                    errors.append(f"{entry.filename}: {type(exc).__name__}: {exc}")
                continue

            meeting_count += 1
            metadata = document.get("metadata", {})
            for field in FIELDS:
                counters[field][_text(metadata.get(field))] += 1

            speakers = document.get("speaker", [])
            declared_speakers = metadata.get("speaker_num")
            try:
                speaker_count = int(declared_speakers)
            except (TypeError, ValueError):
                speaker_count = len(speakers) if isinstance(speakers, list) else 0
            speaker_counts[speaker_count] += 1

            utterances = document.get("utterance", [])
            utterance_count += len(utterances) if isinstance(utterances, list) else 0
            duration_seconds += _duration_seconds(document)
            if isinstance(utterances, list):
                for utterance in utterances:
                    environment = _text(utterance.get("environment"))
                    if environment not in {"(none)", "(empty)"}:
                        environments[environment] += 1

    return {
        "archive": path.name,
        "archive_bytes": path.stat().st_size,
        "uncompressed_bytes": uncompressed_bytes,
        "meetings": meeting_count,
        "utterances": utterance_count,
        "duration_seconds": round(duration_seconds, 3),
        "duration_hours": round(duration_seconds / 3600, 3),
        "speaker_counts": dict(speaker_counts.most_common()),
        "distributions": {
            field: dict(counter.most_common()) for field, counter in counters.items()
        },
        "environments": dict(environments.most_common()),
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze AI-Hub dataset 464 label ZIPs.")
    parser.add_argument("root", type=Path, help="Directory containing TL/VL ZIP files")
    parser.add_argument("--output", type=Path, help="Optional JSON report path")
    args = parser.parse_args()

    zip_paths = sorted(args.root.rglob("*.zip"))
    if not zip_paths:
        parser.error(f"no ZIP files found under {args.root}")

    reports = []
    for index, path in enumerate(zip_paths, start=1):
        print(f"[{index}/{len(zip_paths)}] {path.name}", file=sys.stderr, flush=True)
        reports.append(analyze_zip(path))

    report = {"root": str(args.root), "archives": reports}
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
