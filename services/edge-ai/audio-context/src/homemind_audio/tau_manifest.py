"""Adapter for TAU Sound Events and Speech Privacy Preservation."""

from __future__ import annotations

import csv
import hashlib
import json
from collections import Counter
from pathlib import Path
from typing import Any

from .manifest_ops import MANIFEST_FIELDS


ZENODO_RECORD = "https://zenodo.org/records/10378145"
EVENT_LABELS = {"doorbell": "DOORBELL"}


def _reference_splits(path: str | Path | None) -> dict[str, str]:
    if path is None:
        return {}
    result: dict[str, str] = {}
    with Path(path).resolve().open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            group_id = (row.get("group_id") or "").strip()
            split = (row.get("split") or "").strip().lower()
            if not group_id.startswith("fsd50k:") or split not in {
                "train",
                "validation",
                "test",
            }:
                continue
            previous = result.setdefault(group_id, split)
            if previous != split:
                raise ValueError(
                    f"Reference manifest assigns {group_id!r} to multiple splits"
                )
    return result


def _path_split(path: Path) -> str | None:
    for part in reversed(path.parts):
        value = part.casefold()
        if value == "train":
            return "train"
        if value in {"val", "validation"}:
            return "validation"
        if value in {"eval", "test"}:
            return "test"
    return None


def _stable_split(group_id: str, seed: int) -> str:
    digest = hashlib.sha256(f"{seed}:tau-split:{group_id}".encode()).digest()
    return "validation" if int.from_bytes(digest[:8], "big") % 5 == 0 else "train"


def _rank(row: dict[str, str], seed: int) -> bytes:
    identity = f"{row['label']}:{row['split']}:{row['path']}"
    return hashlib.sha256(f"{seed}:{identity}".encode()).digest()


def build_tau_manifest(
    root: str | Path,
    output: str | Path,
    *,
    metadata: str | Path | None = None,
    reference_manifest: str | Path | None = None,
    max_per_label_per_split: int | None = None,
    include_other: bool = False,
    seed: int = 42,
) -> dict[str, Any]:
    """Build a leakage-aware manifest from the WAV distribution.

    TAU is derived from FSD50K. Evaluation-source rows are deliberately excluded,
    and FSD50K group IDs/splits are reused so transformed copies cannot cross a
    train/validation/test boundary when merged with the original FSD50K manifest.
    """

    root_path = Path(root).resolve()
    output_path = Path(output).resolve()
    metadata_path = (
        Path(metadata).resolve() if metadata is not None else root_path / "metadata.csv"
    )
    if not metadata_path.is_file():
        raise FileNotFoundError(f"TAU metadata not found: {metadata_path}")

    wav_by_stem: dict[str, Path] = {}
    duplicate_stems: set[str] = set()
    for wav_path in root_path.rglob("*.wav"):
        if wav_path.stem in wav_by_stem:
            duplicate_stems.add(wav_path.stem)
        else:
            wav_by_stem[wav_path.stem] = wav_path
    for stem in duplicate_stems:
        wav_by_stem.pop(stem, None)

    reference = _reference_splits(reference_manifest)
    rows: list[dict[str, str]] = []
    missing_audio = 0
    eval_source_excluded = 0
    unsupported_event_excluded = 0
    reference_split_overrides = 0
    with metadata_path.open(encoding="utf-8-sig", newline="") as handle:
        for source_row in csv.DictReader(handle):
            if (source_row.get("split") or "").strip().casefold() == "eval":
                eval_source_excluded += 1
                continue
            event_class = (source_row.get("event_class") or "").strip().casefold()
            label = EVENT_LABELS.get(event_class)
            if label is None:
                if not include_other:
                    unsupported_event_excluded += 1
                    continue
                label = "OTHER"
            file_name = (source_row.get("file_name") or "").strip()
            stem = Path(file_name).stem
            wav_path = wav_by_stem.get(stem)
            if wav_path is None:
                missing_audio += 1
                continue
            original = (source_row.get("original_freesound_file") or "").strip()
            sound_id = Path(original).stem.split("_", 1)[0]
            if not sound_id:
                missing_audio += 1
                continue
            group_id = f"fsd50k:{sound_id}"
            archive_split = _path_split(wav_path)
            split = reference.get(group_id)
            if split == "test":
                # A transformed copy of an FSD50K evaluation clip must never train.
                eval_source_excluded += 1
                continue
            if split is None:
                # The TAU archive can place two segments of one FSD50K clip in
                # train and val. Assign the whole source clip deterministically.
                split = _stable_split(group_id, seed)
                if archive_split is not None and archive_split != split:
                    reference_split_overrides += 1
            elif archive_split is not None and archive_split != split:
                reference_split_overrides += 1
            rows.append(
                {
                    "path": str(wav_path.resolve()),
                    "head": "sound_event",
                    "label": label,
                    "split": split,
                    "start_seconds": "0.000",
                    "end_seconds": "1.000",
                    "group_id": group_id,
                    "dataset": "TAU_SOUND_PRIVACY",
                    "source": "TAU Sound Events and Speech Privacy Preservation",
                    "license": "CC-BY-NC-4.0",
                    "uploader": "TAU / FSD50K / LibriSpeech",
                    "source_url": ZENODO_RECORD,
                }
            )

    if max_per_label_per_split is not None:
        buckets: dict[tuple[str, str], list[dict[str, str]]] = {}
        for row in rows:
            buckets.setdefault((row["label"], row["split"]), []).append(row)
        rows = [
            row
            for key in sorted(buckets)
            for row in sorted(buckets[key], key=lambda item: _rank(item, seed))[
                :max_per_label_per_split
            ]
        ]
    rows.sort(key=lambda row: (row["path"].casefold(), row["label"], row["split"]))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=MANIFEST_FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    counts = Counter(f"{row['label']}/{row['split']}" for row in rows)
    report = {
        "root": str(root_path),
        "metadata": str(metadata_path),
        "manifest": str(output_path),
        "selected_count": len(rows),
        "counts": dict(sorted(counts.items())),
        "unique_fsd50k_groups": len({row["group_id"] for row in rows}),
        "missing_audio_rows": missing_audio,
        "ambiguous_audio_stems": len(duplicate_stems),
        "eval_or_test_source_rows_excluded": eval_source_excluded,
        "unsupported_event_rows_excluded": unsupported_event_excluded,
        "reference_split_overrides": reference_split_overrides,
        "include_other": include_other,
        "license": "CC-BY-NC-4.0",
        "leakage_policy": (
            "Reuse fsd50k:<sound-id> group IDs and reference splits; exclude all "
            "FSD50K evaluation-derived samples from this augmentation manifest."
        ),
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report
