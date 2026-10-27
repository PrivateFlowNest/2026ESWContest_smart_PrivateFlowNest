"""FSD50K adapter for HomeMind doorbell and alarm sound-event classes."""

from __future__ import annotations

import csv
import hashlib
import json
import wave
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any


TARGET_NAMES = {"doorbell", "ding_dong"}
ALARM_NAMES = {
    "alarm",
    "fire_alarm",
    "alarm_clock",
    "car_alarm",
    "smoke_detector_smoke_alarm",
    "siren",
    "buzzer",
}
OTHER_NAMES = {
    "bell",
    "chime",
    "ringtone",
    "telephone",
    "knock",
    "door",
    "microwave_oven",
    "mechanical_fan",
    "domestic_sounds_home_sounds",
}
FIELDS = (
    "path",
    "head",
    "label",
    "split",
    "start_seconds",
    "end_seconds",
    "group_id",
    "dataset",
    "source",
    "license",
    "uploader",
    "source_url",
)


@dataclass(frozen=True)
class FsdCandidate:
    path: Path
    label: str
    split: str
    start_seconds: float
    end_seconds: float
    fname: str
    license: str
    uploader: str

    def row(self) -> dict[str, str]:
        return {
            "path": str(self.path.resolve()),
            "head": "sound_event",
            "label": self.label,
            "split": self.split,
            "start_seconds": f"{self.start_seconds:.3f}",
            "end_seconds": f"{self.end_seconds:.3f}",
            "group_id": f"fsd50k:{self.fname}",
            "dataset": "FSD50K",
            "source": "FSD50K v1.0 / Zenodo 4060432",
            "license": self.license,
            "uploader": self.uploader,
            "source_url": f"https://freesound.org/s/{self.fname}/",
        }


def _normalized_labels(value: str) -> set[str]:
    return {
        label.strip().lower().replace(" ", "_").replace(",", "")
        for label in value.split(",")
        if label.strip()
    }


def map_fsd50k_label(labels: str) -> str | None:
    names = _normalized_labels(labels)
    if names & TARGET_NAMES:
        return "DOORBELL"
    if names & ALARM_NAMES:
        return "ALARM"
    if names & OTHER_NAMES:
        return "OTHER"
    return None


def _duration(path: Path) -> float:
    with wave.open(str(path), "rb") as handle:
        return handle.getnframes() / handle.getframerate()


def _metadata_info(root: Path, partition: str) -> dict[str, dict[str, str]]:
    path = root / "FSD50K.metadata" / f"{partition}_clips_info_FSD50K.json"
    if not path.is_file():
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    return {
        str(fname): {
            "license": str(info.get("license", "UNKNOWN")),
            "uploader": str(info.get("uploader", "UNKNOWN")),
        }
        for fname, info in document.items()
        if isinstance(info, dict)
    }


def _license_allowed(license_name: str) -> bool:
    normalized = license_name.lower()
    return (
        normalized in {"cc0", "cc-by", "cc by"}
        or "publicdomain/zero" in normalized
        or "/licenses/by/" in normalized
    )


def _collect_partition(
    root: Path, partition: str, *, allow_restricted_licenses: bool
) -> tuple[list[FsdCandidate], int]:
    ground_truth = root / "FSD50K.ground_truth"
    csv_path = ground_truth / f"{partition}.csv"
    audio_root = root / f"FSD50K.{partition}_audio"
    if not csv_path.is_file() or not audio_root.is_dir():
        return [], 0
    metadata = _metadata_info(root, partition)
    candidates: list[FsdCandidate] = []
    restricted_count = 0
    with csv_path.open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            fname = str(row.get("fname", "")).strip()
            label = map_fsd50k_label(str(row.get("labels", "")))
            wav_path = audio_root / f"{fname}.wav"
            if not fname or not label or not wav_path.is_file():
                continue
            clip_info = metadata.get(
                fname, {"license": "UNKNOWN", "uploader": "UNKNOWN"}
            )
            if not allow_restricted_licenses and not _license_allowed(
                clip_info["license"]
            ):
                restricted_count += 1
                continue
            try:
                duration = _duration(wav_path)
            except (OSError, EOFError, wave.Error):
                continue
            if duration <= 0:
                continue
            split = "test" if partition == "eval" else str(row.get("split", "train"))
            split = "validation" if split in {"val", "validation"} else split
            if split not in {"train", "validation", "test"}:
                continue
            candidates.append(
                FsdCandidate(
                    wav_path,
                    label,
                    split,
                    0.0,
                    min(duration, 5.0),
                    fname,
                    clip_info["license"],
                    clip_info["uploader"],
                )
            )
    return candidates, restricted_count


def build_fsd50k_manifest(
    root: str | Path,
    output: str | Path,
    *,
    max_per_label_per_split: int | None = 2_000,
    seed: int = 42,
    allow_restricted_licenses: bool = False,
) -> dict[str, Any]:
    root_path = Path(root).resolve()
    output_path = Path(output).resolve()
    dev_candidates, dev_restricted = _collect_partition(
        root_path, "dev", allow_restricted_licenses=allow_restricted_licenses
    )
    eval_candidates, eval_restricted = _collect_partition(
        root_path, "eval", allow_restricted_licenses=allow_restricted_licenses
    )
    candidates = dev_candidates + eval_candidates
    buckets: dict[tuple[str, str], list[FsdCandidate]] = {}
    for candidate in candidates:
        buckets.setdefault((candidate.label, candidate.split), []).append(candidate)
    selected: list[FsdCandidate] = []
    for (label, split), bucket in buckets.items():
        ranked = sorted(
            bucket,
            key=lambda item: hashlib.sha256(
                f"{seed}:{label}:{split}:{item.fname}".encode()
            ).digest(),
        )
        selected.extend(
            ranked
            if max_per_label_per_split is None
            else ranked[:max_per_label_per_split]
        )
    selected.sort(key=lambda item: (item.label, item.split, item.fname))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(candidate.row() for candidate in selected)

    counts = Counter(f"{item.label}/{item.split}" for item in selected)
    report = {
        "root": str(root_path),
        "manifest": str(output_path),
        "selected_count": len(selected),
        "counts": dict(sorted(counts.items())),
        "license_unknown_count": sum(item.license == "UNKNOWN" for item in selected),
        "restricted_license_excluded_count": dev_restricted + eval_restricted,
        "allow_restricted_licenses": allow_restricted_licenses,
        "license_notice": (
            "Retain per-clip Creative Commons license metadata and review the FSD50K "
            "competition-use notice before final submission."
        ),
        "unsupported_target": "WASHER_COMPLETE",
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report
