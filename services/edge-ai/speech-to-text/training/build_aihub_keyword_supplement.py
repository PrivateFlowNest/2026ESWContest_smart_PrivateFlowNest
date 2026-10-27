"""Mine local AI Hub Training audio for HomeMind vocabulary adaptation.

Only utterances with a matching, already-extracted audio file are emitted.  Long
recordings remain in place and are represented by start/end offsets so that no
large duplicate audio tree has to be created.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import heapq
import json
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any, Iterable
import zipfile


PATTERNS: dict[str, re.Pattern[str]] = {
    "light": re.compile(r"조명|전등|불빛|불\s*(?:을|이|은|도|좀)?\s*(?:켜|꺼|끄|밝|어둡)"),
    "tv": re.compile(r"(?:^|\s)(?:tv|티비|티브이|텔레비전|텔레비젼)(?:\s|$)", re.I),
    "aircon": re.compile(r"에어컨|에어콘|냉방"),
    "front_door": re.compile(r"현관문|현관\s*문"),
    "bathroom": re.compile(r"화장실"),
    "meeting": re.compile(r"회의|미팅"),
    "sound": re.compile(r"소리|볼륨|음량"),
    "ventilation": re.compile(r"환기팬|환풍기|환기\s*팬|환풍"),
}


def clean_text(value: Any) -> str:
    text = str(value or "").strip()
    return re.sub(r"\s+", " ", text)


def matched_groups(text: str) -> list[str]:
    return [name for name, pattern in PATTERNS.items() if pattern.search(text)]


def nested_value(value: Any, key: str) -> Any:
    if isinstance(value, dict):
        if key in value:
            return value[key]
        for child in value.values():
            found = nested_value(child, key)
            if found is not None:
                return found
    elif isinstance(value, list):
        for child in value:
            found = nested_value(child, key)
            if found is not None:
                return found
    return None


def seconds(value: Any) -> float | None:
    try:
        return float(value)
    except (TypeError, ValueError):
        pass
    if isinstance(value, str) and ":" in value:
        try:
            parts = [float(part) for part in value.split(":")]
            result = 0.0
            for part in parts:
                result = result * 60.0 + part
            return result
        except ValueError:
            return None
    return None


class BalancedReservoir:
    """Keep deterministic, bounded samples for every keyword group."""

    def __init__(self, per_group: int) -> None:
        self.per_group = per_group
        self.heaps: dict[str, list[tuple[int, str, dict[str, Any]]]] = {
            name: [] for name in PATTERNS
        }
        self.seen: Counter[str] = Counter()

    def add(self, row: dict[str, Any], groups: list[str]) -> None:
        row["keyword_groups"] = groups
        row_id = str(row["id"])
        score = int.from_bytes(
            hashlib.blake2b(row_id.encode("utf-8"), digest_size=8).digest(), "big"
        )
        for group in groups:
            self.seen[group] += 1
            item = (score, row_id, row)
            heap = self.heaps[group]
            if len(heap) < self.per_group:
                heapq.heappush(heap, item)
            elif score > heap[0][0]:
                heapq.heapreplace(heap, item)

    def rows(self) -> list[dict[str, Any]]:
        unique: dict[str, dict[str, Any]] = {}
        for heap in self.heaps.values():
            for _, row_id, row in heap:
                unique[row_id] = row
        return sorted(unique.values(), key=lambda row: str(row["id"]))


def zip_jsons(path: Path) -> Iterable[tuple[str, dict[str, Any]]]:
    with zipfile.ZipFile(path) as archive:
        names = [name for name in archive.namelist() if name.lower().endswith(".json")]
        for index, name in enumerate(names, 1):
            if index % 25_000 == 0:
                print(f"  {path.name}: {index:,}/{len(names):,}", file=sys.stderr, flush=True)
            try:
                yield name, json.loads(archive.read(name))
            except (UnicodeDecodeError, json.JSONDecodeError, KeyError):
                continue


def add_short_utterances(root: Path, reservoir: BalancedReservoir) -> Counter[str]:
    stats: Counter[str] = Counter()
    dataset = root / "109_free_conversation"
    labels = list((dataset / "archives" / "labels").rglob("[[]라벨]2.음성수집도구.zip"))
    source_roots = sorted((dataset / "extracted" / "training").glob("[[]원천]2.음성수집도구*"))
    if not labels or not source_roots:
        return stats
    folder_roots: dict[str, Path] = {}
    for source_root in source_roots:
        for folder in source_root.iterdir():
            if folder.is_dir():
                folder_roots[folder.name] = source_root
    print(f"Scanning AIHUB-109: {labels[0].name}", file=sys.stderr, flush=True)
    for member, payload in zip_jsons(labels[0]):
        stats["labels"] += 1
        text = clean_text(nested_value(payload, "stt"))
        groups = matched_groups(text)
        if not groups:
            continue
        relative = PurePosixPath(member.lstrip("/\\")).with_suffix(".wav")
        source_root = folder_roots.get(relative.parts[0]) if relative.parts else None
        if source_root is None:
            stats["missing_audio"] += 1
            continue
        audio_path = source_root.joinpath(*relative.parts)
        if not audio_path.is_file():
            stats["missing_audio"] += 1
            continue
        row_id = f"AIHUB109:{relative.as_posix()}"
        reservoir.add(
            {
                "id": row_id,
                "audio_path": str(audio_path),
                "reference": text,
                "source": "AIHUB-109-TRAINING-VOICE-COLLECTION",
                "privacy_scope": "LOCAL_TRAINING_ONLY",
            },
            groups,
        )
        stats["matched"] += 1
    return stats


def add_segmented_archive(
    archive_path: Path,
    source_root: Path,
    source_name: str,
    reservoir: BalancedReservoir,
) -> Counter[str]:
    stats: Counter[str] = Counter()
    print(f"Scanning {source_name}: {archive_path.name}", file=sys.stderr, flush=True)
    for member, payload in zip_jsons(archive_path):
        stats["labels"] += 1
        relative = PurePosixPath(member.lstrip("/\\")).with_suffix(".wav")
        audio_path = source_root.joinpath(*relative.parts)
        if not audio_path.is_file():
            stats["missing_audio"] += 1
            continue
        utterances = payload.get("utterance")
        if not isinstance(utterances, list):
            continue
        for utterance in utterances:
            text = clean_text(utterance.get("form") or utterance.get("original_form"))
            groups = matched_groups(text)
            if not groups:
                continue
            start = seconds(utterance.get("start"))
            end = seconds(utterance.get("end"))
            if start is None or end is None or not (0.2 <= end - start <= 30.0):
                stats["invalid_segment"] += 1
                continue
            utterance_id = clean_text(utterance.get("id")) or f"{start:.3f}-{end:.3f}"
            reservoir.add(
                {
                    "id": f"{source_name}:{relative.as_posix()}:{utterance_id}",
                    "audio_path": str(audio_path),
                    "reference": text,
                    "start_seconds": start,
                    "end_seconds": end,
                    "duration_seconds": round(end - start, 3),
                    "source": source_name,
                    "privacy_scope": "LOCAL_TRAINING_ONLY",
                },
                groups,
            )
            stats["matched"] += 1
    return stats


def add_broadcast_and_meeting(root: Path, reservoir: BalancedReservoir) -> dict[str, Counter[str]]:
    output: dict[str, Counter[str]] = {}
    for dataset_name, source_name, numbers in (
        ("463_broadcast", "AIHUB-463-TRAINING", (1, 2, 4)),
        ("464_meeting", "AIHUB-464-TRAINING", (2, 4)),
    ):
        dataset = root / dataset_name
        label_root = dataset / "archives" / "labels"
        source_base = dataset / "extracted" / "training"
        total: Counter[str] = Counter()
        for number in numbers:
            labels = list(label_root.rglob(f"TL{number}.zip"))
            source_root = source_base / f"TS{number}"
            if labels and source_root.is_dir():
                total.update(add_segmented_archive(labels[0], source_root, source_name, reservoir))
        output[source_name] = total
    return output


def add_residential_dialogue(root: Path, reservoir: BalancedReservoir) -> Counter[str]:
    stats: Counter[str] = Counter()
    dataset = root / "71698_environment_sound"
    source_base = dataset / "extracted" / "training"
    label_base = dataset / "archives" / "labels"
    archives_by_name = {path.name: path for path in label_base.rglob("TL_대화음성데이터_*.zip")}
    for source_root in sorted(source_base.glob("TS_대화음성데이터_*")):
        label_name = "TL_" + source_root.name.removeprefix("TS_") + ".zip"
        archive_path = archives_by_name.get(label_name)
        if archive_path is None:
            continue
        print(f"Scanning AIHUB-71698: {archive_path.name}", file=sys.stderr, flush=True)
        for member, payload in zip_jsons(archive_path):
            stats["labels"] += 1
            relative = PurePosixPath(member.lstrip("/\\")).with_suffix(".wav")
            audio_path = source_root.joinpath(*relative.parts)
            if not audio_path.is_file():
                stats["missing_audio"] += 1
                continue
            dialogs = payload.get("Dialogs")
            if not isinstance(dialogs, list):
                continue
            for dialog in dialogs:
                text = clean_text(dialog.get("SpeakerText") or dialog.get("Speakertext"))
                groups = matched_groups(text)
                if not groups:
                    continue
                start = seconds(dialog.get("StartTime"))
                end = seconds(dialog.get("EndTime"))
                if start is None or end is None or not (0.2 <= end - start <= 30.0):
                    stats["invalid_segment"] += 1
                    continue
                dialog_id = dialog.get("DialogNum", f"{start:.3f}-{end:.3f}")
                reservoir.add(
                    {
                        "id": f"AIHUB71698:{relative.as_posix()}:{dialog_id}",
                        "audio_path": str(audio_path),
                        "reference": text,
                        "start_seconds": start,
                        "end_seconds": end,
                        "duration_seconds": round(end - start, 3),
                        "source": "AIHUB-71698-TRAINING-RESIDENTIAL-DIALOGUE",
                        "privacy_scope": "LOCAL_TRAINING_ONLY",
                    },
                    groups,
                )
                stats["matched"] += 1
    return stats


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--max-per-group", type=int, default=600)
    args = parser.parse_args()

    reservoir = BalancedReservoir(args.max_per_group)
    scan_stats: dict[str, Counter[str]] = {
        "AIHUB-109-TRAINING-VOICE-COLLECTION": add_short_utterances(args.root, reservoir)
    }
    scan_stats.update(add_broadcast_and_meeting(args.root, reservoir))
    scan_stats["AIHUB-71698-TRAINING-RESIDENTIAL-DIALOGUE"] = add_residential_dialogue(
        args.root, reservoir
    )
    rows = reservoir.rows()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    selected_counts: Counter[str] = Counter()
    source_counts: Counter[str] = Counter()
    for row in rows:
        selected_counts.update(row["keyword_groups"])
        source_counts[str(row["source"])] += 1
    summary = {
        "output": str(args.output),
        "selected_rows": len(rows),
        "max_per_keyword_group": args.max_per_group,
        "candidate_keyword_counts": dict(reservoir.seen),
        "selected_keyword_counts": dict(selected_counts),
        "selected_source_counts": dict(source_counts),
        "scan_stats": {name: dict(stats) for name, stats in scan_stats.items()},
        "split": "Training only",
        "validation_rows_included": 0,
        "raw_transcripts_in_summary": False,
    }
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
