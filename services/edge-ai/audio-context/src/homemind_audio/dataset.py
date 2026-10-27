"""Training manifest parser."""

from __future__ import annotations

import csv
import os
import re
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from .labels import HEAD_LABELS


WINDOWS_ABSOLUTE_PATH = re.compile(r"^(?P<drive>[A-Za-z]):[\\/](?P<rest>.*)$")


@dataclass(frozen=True)
class AudioRecord:
    path: Path
    head: str
    label: str
    split: str
    start_seconds: float | None = None
    end_seconds: float | None = None
    group_id: str | None = None
    dataset: str | None = None


def manifest_audio_path(
    value: str,
    manifest_parent: Path,
    *,
    platform_name: str | None = None,
    windows_mount_root: Path | None = None,
) -> Path:
    """Translate Windows absolute paths when a manifest is read under WSL/Linux."""

    platform = platform_name or os.name
    match = WINDOWS_ABSOLUTE_PATH.match(value)
    if match and platform != "nt":
        mount_root = windows_mount_root or Path(
            os.environ.get("HOMEMIND_WINDOWS_MOUNT_ROOT", "/mnt")
        )
        rest = match.group("rest").replace("\\", "/")
        return mount_root / match.group("drive").lower() / Path(rest)
    row_path = Path(value)
    return row_path if row_path.is_absolute() else manifest_parent / row_path


def iter_manifest(
    path: str | Path, *, check_files: bool = True
) -> Iterator[AudioRecord]:
    """Yield validated records without retaining a large manifest in memory."""

    manifest_path = Path(path).resolve()
    record_count = 0

    with manifest_path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {"path", "head", "label", "split"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError(f"Manifest columns must include {sorted(required)}")

        for line_number, row in enumerate(reader, start=2):
            head = row["head"].strip()
            label = row["label"].strip().upper()
            split = row["split"].strip().lower()
            audio_path = manifest_audio_path(
                row["path"].strip(), manifest_path.parent
            )
            start_text = (row.get("start_seconds") or "").strip()
            end_text = (row.get("end_seconds") or "").strip()
            start_seconds = float(start_text) if start_text else None
            end_seconds = float(end_text) if end_text else None

            if head not in HEAD_LABELS:
                raise ValueError(f"Line {line_number}: unknown head {head!r}")
            if label not in HEAD_LABELS[head]:
                raise ValueError(
                    f"Line {line_number}: invalid label {label!r} for {head!r}"
                )
            if split not in {"train", "validation", "test"}:
                raise ValueError(f"Line {line_number}: invalid split {split!r}")
            if check_files:
                audio_path = audio_path.resolve()
                if not audio_path.is_file():
                    raise FileNotFoundError(f"Line {line_number}: {audio_path}")
            if (start_seconds is None) != (end_seconds is None):
                raise ValueError(
                    f"Line {line_number}: start_seconds and end_seconds must be paired"
                )
            if start_seconds is not None and (
                start_seconds < 0 or end_seconds <= start_seconds
            ):
                raise ValueError(f"Line {line_number}: invalid audio segment")

            record_count += 1
            yield AudioRecord(
                audio_path,
                head,
                label,
                split,
                start_seconds,
                end_seconds,
                (row.get("group_id") or "").strip() or None,
                (row.get("dataset") or "").strip() or None,
            )

    if not record_count:
        raise ValueError("Manifest contains no audio records")


def load_manifest(path: str | Path) -> list[AudioRecord]:
    return list(iter_manifest(path))
