"""Final-model dataset readiness checks for the HomeMind audio heads."""

from __future__ import annotations

from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Callable

from .dataset import iter_manifest


FINAL_REQUIRED_LABELS = {
    "activity": (
        "QUIET",
        "SELF_TALK",
        "TV_MEDIA",
        "ZOOM_SPEAKER",
        "ZOOM_HEADPHONES",
        "IN_PERSON_MEETING",
    ),
    "sound_event": (
        "DOORBELL",
        "WASHER_COMPLETE",
        "ALARM",
        "OTHER",
    ),
}


def assess_final_readiness(
    manifest_path: str | Path,
    *,
    minimum_train_per_label: int = 80,
    minimum_validation_per_label: int = 15,
    minimum_test_per_label: int = 20,
    check_files: bool = True,
    progress: Callable[[str], None] | None = None,
) -> dict[str, Any]:
    """Report whether a manifest is structurally ready for final training."""

    minimums = {
        "train": minimum_train_per_label,
        "validation": minimum_validation_per_label,
        "test": minimum_test_per_label,
    }
    if any(value < 1 for value in minimums.values()):
        raise ValueError("Final readiness minimums must be positive")

    sample_counts: Counter[tuple[str, str, str]] = Counter()
    unique_groups: set[tuple[str, str, str, str]] = set()
    group_splits: dict[str, set[str]] = defaultdict(set)
    missing_group_ids = 0
    record_count = 0
    report_progress = progress or (lambda _message: None)
    for record in iter_manifest(manifest_path, check_files=check_files):
        record_count += 1
        sample_counts[(record.head, record.label, record.split)] += 1
        if record.group_id:
            group_splits[record.group_id].add(record.split)
            unique_groups.add(
                (record.head, record.label, record.split, record.group_id)
            )
        else:
            missing_group_ids += 1
        if record_count % 100_000 == 0:
            report_progress(f"Validated {record_count:,} manifest rows")

    counts = Counter((head, label, split) for head, label, split, _ in unique_groups)

    group_leaks = sorted(
        group_id for group_id, splits in group_splits.items() if len(splits) > 1
    )
    gaps: list[dict[str, Any]] = []
    for head, labels in FINAL_REQUIRED_LABELS.items():
        for label in labels:
            for split, minimum in minimums.items():
                actual = counts[(head, label, split)]
                if actual < minimum:
                    gaps.append(
                        {
                            "head": head,
                            "label": label,
                            "split": split,
                            "actual": actual,
                            "required": minimum,
                            "missing": minimum - actual,
                        }
                    )

    blockers: list[str] = []
    if gaps:
        blockers.append("required_label_coverage")
    if group_leaks:
        blockers.append("group_split_leakage")
    if missing_group_ids:
        blockers.append("missing_group_ids")

    rendered_counts = {
        f"{head}/{label}/{split}": count
        for (head, label, split), count in sorted(counts.items())
    }
    rendered_sample_counts = {
        f"{head}/{label}/{split}": count
        for (head, label, split), count in sorted(sample_counts.items())
    }
    return {
        "manifest": str(Path(manifest_path).resolve()),
        "ready_for_final_training": not blockers,
        "file_existence_checked": check_files,
        "record_count": record_count,
        "minimums_per_required_label": minimums,
        "counts": rendered_counts,
        "sample_counts": rendered_sample_counts,
        "counting_rule": "unique group_id per head, label, and split",
        "coverage_gaps": gaps,
        "group_leak_count": len(group_leaks),
        "group_leaks": group_leaks[:100],
        "missing_group_id_count": missing_group_ids,
        "blockers": blockers,
        "metric_gate_after_training": {
            "activity_macro_f1": 0.90,
            "sound_event_macro_f1": 0.90,
            "report_confusion_matrix": True,
            "report_false_positive_rate": True,
        },
        "note": (
            "This gate checks data structure and coverage. A model is final only after "
            "the independent test metric gate and Pi 5 latency checks also pass."
        ),
    }
