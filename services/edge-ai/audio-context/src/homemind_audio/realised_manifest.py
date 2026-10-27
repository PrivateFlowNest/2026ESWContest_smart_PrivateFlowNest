"""Hard-negative adapter for the ReaLISED indoor sound dataset."""

from __future__ import annotations

import csv
import hashlib
import json
from collections import Counter
from pathlib import Path
from typing import Any

from .manifest_ops import MANIFEST_FIELDS


ZENODO_RECORD = "https://zenodo.org/records/6488321"
CLASS_PREFIXES = {
    "bea",
    "coo",
    "cup",
    "dis",
    "doo",
    "dra",
    "fur",
    "mic",
    "obj",
    "smo",
    "spe",
    "swi",
    "tel",
    "vac",
    "wal",
    "was",
    "wat",
    "win",
}


def _split(group_id: str, seed: int) -> str:
    digest = hashlib.sha256(f"{seed}:realised:{group_id}".encode()).digest()
    return "validation" if int.from_bytes(digest[:8], "big") % 5 == 0 else "train"


def build_realised_manifest(
    root: str | Path,
    output: str | Path,
    *,
    max_per_split: int | None = None,
    seed: int = 42,
) -> dict[str, Any]:
    """Map ReaLISED to OTHER, never to WASHER_COMPLETE.

    ReaLISED washing-machine recordings describe appliance operation, not the
    completion chime. They are therefore useful as hard negatives only.
    """

    root_path = Path(root).resolve()
    output_path = Path(output).resolve()
    rows: list[dict[str, str]] = []
    ignored_files = 0
    class_counts: Counter[str] = Counter()
    for audio_path in sorted(root_path.rglob("*.flac")):
        prefix = audio_path.stem.split("_", 1)[0].casefold()
        if prefix not in CLASS_PREFIXES:
            ignored_files += 1
            continue
        group_id = f"realised:{audio_path.stem}"
        split = _split(group_id, seed)
        class_counts[f"{prefix}/{split}"] += 1
        rows.append(
            {
                "path": str(audio_path.resolve()),
                "head": "sound_event",
                "label": "OTHER",
                "split": split,
                "start_seconds": "",
                "end_seconds": "",
                "group_id": group_id,
                "dataset": "ReaLISED",
                "source": "Real-Life Indoor Sound Event Dataset",
                "license": "CC-BY-4.0",
                "uploader": "University of Alcala / INTA",
                "source_url": ZENODO_RECORD,
            }
        )
    if max_per_split is not None:
        selected: list[dict[str, str]] = []
        for split in ("train", "validation"):
            bucket = [row for row in rows if row["split"] == split]
            bucket.sort(
                key=lambda row: hashlib.sha256(
                    f"{seed}:{row['group_id']}".encode()
                ).digest()
            )
            selected.extend(bucket[:max_per_split])
        rows = selected
    rows.sort(key=lambda row: row["path"].casefold())
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=MANIFEST_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    counts = Counter(f"OTHER/{row['split']}" for row in rows)
    report = {
        "root": str(root_path),
        "manifest": str(output_path),
        "selected_count": len(rows),
        "counts": dict(sorted(counts.items())),
        "source_class_counts": dict(sorted(class_counts.items())),
        "ignored_files": ignored_files,
        "license": "CC-BY-4.0",
        "label_policy": (
            "All clips are OTHER hard negatives. Washing-machine operation is "
            "explicitly not treated as WASHER_COMPLETE."
        ),
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report
