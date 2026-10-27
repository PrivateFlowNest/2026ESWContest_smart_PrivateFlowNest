"""Streaming operations for large HomeMind training manifests."""

from __future__ import annotations

import csv
from collections import deque
import heapq
import hashlib
import json
import os
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


MANIFEST_FIELDS = (
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
REQUIRED_FIELDS = frozenset({"path", "head", "label", "split", "group_id"})
IDENTITY_FIELDS = (
    "path",
    "head",
    "label",
    "split",
    "start_seconds",
    "end_seconds",
    "group_id",
)


def _row_fingerprint(row: dict[str, str]) -> bytes:
    identity = "\x1f".join(row[field] for field in IDENTITY_FIELDS)
    return hashlib.blake2b(identity.encode("utf-8"), digest_size=16).digest()


def _stable_rank(row: dict[str, str], seed: int) -> int:
    identity = "\x1f".join(row[field] for field in IDENTITY_FIELDS)
    digest = hashlib.sha256(f"{seed}:{identity}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


def _group_rank(group_id: str, seed: int) -> bytes:
    return hashlib.sha256(f"{seed}:group:{group_id}".encode("utf-8")).digest()


def windowize_manifest(
    input_manifest: str | Path,
    output: str | Path,
    *,
    head: str = "activity",
    window_seconds: float = 4.0,
    hop_seconds: float = 2.0,
    short_policy: str = "drop",
) -> dict[str, Any]:
    """Rewrite one head into fixed windows while passing other heads through.

    Fixed windows keep their source ``group_id`` and split, so a clip can never
    leak from training into validation just because it produced several windows.
    Rows without an explicit time range are passed through for non-target heads.
    """

    if window_seconds <= 0 or hop_seconds <= 0:
        raise ValueError("window_seconds and hop_seconds must be positive")
    if hop_seconds > window_seconds:
        raise ValueError("hop_seconds must not exceed window_seconds")
    if short_policy not in {"drop", "keep"}:
        raise ValueError("short_policy must be 'drop' or 'keep'")

    input_path = Path(input_manifest).resolve()
    output_path = Path(output).resolve()
    if output_path == input_path:
        raise ValueError("Output manifest must not overwrite the input manifest")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f"{output_path.name}.tmp")
    counts: Counter[str] = Counter()
    sample_counts: Counter[str] = Counter()
    seen: set[bytes] = set()

    try:
        with input_path.open("r", encoding="utf-8-sig", newline="") as source:
            reader = csv.DictReader(source)
            fields = set(reader.fieldnames or ())
            if not REQUIRED_FIELDS.issubset(fields):
                missing = sorted(REQUIRED_FIELDS - fields)
                raise ValueError(f"Manifest {input_path} is missing columns: {missing}")

            with temporary_path.open("w", encoding="utf-8-sig", newline="") as target:
                writer = csv.DictWriter(target, fieldnames=MANIFEST_FIELDS)
                writer.writeheader()

                for line_number, source_row in enumerate(reader, start=2):
                    counts["source_rows"] += 1
                    row = {
                        field: (source_row.get(field) or "").strip()
                        for field in MANIFEST_FIELDS
                    }
                    emitted: list[dict[str, str]]
                    if row["head"] != head:
                        counts["passthrough_rows"] += 1
                        emitted = [row]
                    else:
                        start_text = row["start_seconds"]
                        end_text = row["end_seconds"]
                        if not start_text or not end_text:
                            raise ValueError(
                                f"Manifest row {line_number} for {head!r} has no time range"
                            )
                        start = float(start_text)
                        end = float(end_text)
                        if start < 0 or end <= start:
                            raise ValueError(
                                f"Manifest row {line_number} has an invalid time range"
                            )

                        duration = end - start
                        if duration + 1e-9 < window_seconds:
                            counts["short_rows"] += 1
                            if short_policy == "drop":
                                counts["short_rows_dropped"] += 1
                                emitted = []
                            else:
                                counts["short_rows_kept"] += 1
                                emitted = [row]
                        else:
                            emitted = []
                            window_start = start
                            while window_start + window_seconds <= end + 1e-9:
                                window_end = window_start + window_seconds
                                windowed = dict(row)
                                windowed["start_seconds"] = f"{window_start:.3f}"
                                windowed["end_seconds"] = f"{window_end:.3f}"
                                emitted.append(windowed)
                                window_start += hop_seconds
                            counts["target_rows_windowed"] += 1

                    for candidate in emitted:
                        fingerprint = _row_fingerprint(candidate)
                        if fingerprint in seen:
                            counts["duplicate_rows_skipped"] += 1
                            continue
                        seen.add(fingerprint)
                        writer.writerow(candidate)
                        counts["output_rows"] += 1
                        if candidate["head"] == head:
                            counts["target_windows"] += 1
                        sample_counts[
                            f"{candidate['head']}/{candidate['label']}/{candidate['split']}"
                        ] += 1

        os.replace(temporary_path, output_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise

    report = {
        "manifest": str(output_path),
        "input": str(input_path),
        "target_head": head,
        "window_seconds": window_seconds,
        "hop_seconds": hop_seconds,
        "short_policy": short_policy,
        **dict(counts),
        "sample_counts": dict(sorted(sample_counts.items())),
        "split_safety": "source group_id and split preserved for every derived window",
        "output_order": "source manifest order, then ascending window start",
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report


def reserve_validation_groups(
    input_manifest: str | Path,
    output: str | Path,
    *,
    head: str = "activity",
    groups_per_label: int = 100,
    seed: int = 20260903,
) -> dict[str, Any]:
    """Move whole validation source groups to test before model selection."""

    if groups_per_label < 1:
        raise ValueError("groups_per_label must be positive")
    input_path = Path(input_manifest).resolve()
    output_path = Path(output).resolve()
    if input_path == output_path:
        raise ValueError("Output manifest must not overwrite the input manifest")

    rows: list[dict[str, str]] = []
    validation_groups: dict[str, set[str]] = {}
    with input_path.open("r", encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        fields = set(reader.fieldnames or ())
        if not REQUIRED_FIELDS.issubset(fields):
            missing = sorted(REQUIRED_FIELDS - fields)
            raise ValueError(f"Manifest {input_path} is missing columns: {missing}")
        for line_number, source_row in enumerate(reader, start=2):
            row = {
                field: (source_row.get(field) or "").strip()
                for field in MANIFEST_FIELDS
            }
            if not row["group_id"]:
                raise ValueError(f"Manifest row {line_number} has no group_id")
            rows.append(row)
            if row["head"] == head and row["split"] == "validation":
                validation_groups.setdefault(row["label"], set()).add(row["group_id"])

    reserved: dict[str, set[str]] = {}
    for label, groups in sorted(validation_groups.items()):
        if len(groups) < groups_per_label:
            raise ValueError(
                f"{label} has {len(groups)} validation groups; "
                f"{groups_per_label} required"
            )
        ordered = sorted(
            groups,
            key=lambda group_id: hashlib.sha256(
                f"{seed}:test:{head}:{label}:{group_id}".encode("utf-8")
            ).digest(),
        )
        reserved[label] = set(ordered[:groups_per_label])

    moved_rows = 0
    for row in rows:
        if (
            row["head"] == head
            and row["split"] == "validation"
            and row["group_id"] in reserved.get(row["label"], set())
        ):
            row["split"] = "test"
            moved_rows += 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f"{output_path.name}.tmp")
    try:
        with temporary_path.open("w", encoding="utf-8-sig", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=MANIFEST_FIELDS)
            writer.writeheader()
            writer.writerows(rows)
        os.replace(temporary_path, output_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise

    validation_after = {
        label: len(groups - reserved[label])
        for label, groups in validation_groups.items()
    }
    report = {
        "manifest": str(output_path),
        "input": str(input_path),
        "target_head": head,
        "seed": seed,
        "reserved_test_groups_per_label": {
            label: len(groups) for label, groups in sorted(reserved.items())
        },
        "remaining_validation_groups_per_label": dict(sorted(validation_after.items())),
        "moved_rows": moved_rows,
        "group_overlap_count": 0,
        "selection_rule": "stable hash; all rows from a source group move together",
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report


def merge_manifests(
    inputs: Iterable[str | Path], output: str | Path
) -> dict[str, Any]:
    """Merge manifests without loading their rows into memory.

    The output uses the union schema needed by AI Hub and FSD50K. Exact duplicate
    rows are omitted while per-clip FSD50K attribution and license fields are kept.
    """

    input_paths = [Path(path).resolve() for path in inputs]
    if len(input_paths) < 2:
        raise ValueError("At least two input manifests are required")
    output_path = Path(output).resolve()
    if output_path in input_paths:
        raise ValueError("Output manifest must not overwrite an input manifest")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f"{output_path.name}.tmp")
    counts: Counter[str] = Counter()
    input_counts: Counter[str] = Counter()
    seen: set[bytes] = set()
    duplicate_count = 0
    selected_count = 0

    try:
        with temporary_path.open("w", encoding="utf-8-sig", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=MANIFEST_FIELDS)
            writer.writeheader()

            for input_path in input_paths:
                with input_path.open("r", encoding="utf-8-sig", newline="") as source:
                    reader = csv.DictReader(source)
                    fields = set(reader.fieldnames or ())
                    if not REQUIRED_FIELDS.issubset(fields):
                        missing = sorted(REQUIRED_FIELDS - fields)
                        raise ValueError(
                            f"Manifest {input_path} is missing columns: {missing}"
                        )

                    for source_row in reader:
                        row = {
                            field: (source_row.get(field) or "").strip()
                            for field in MANIFEST_FIELDS
                        }
                        fingerprint = _row_fingerprint(row)
                        if fingerprint in seen:
                            duplicate_count += 1
                            continue
                        seen.add(fingerprint)
                        writer.writerow(row)
                        selected_count += 1
                        input_counts[str(input_path)] += 1
                        counts[f"{row['head']}/{row['label']}/{row['split']}"] += 1

        os.replace(temporary_path, output_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise

    report = {
        "manifest": str(output_path),
        "inputs": [str(path) for path in input_paths],
        "selected_count": selected_count,
        "exact_duplicate_rows_skipped": duplicate_count,
        "input_counts": dict(input_counts),
        "sample_counts": dict(sorted(counts.items())),
        "schema": list(MANIFEST_FIELDS),
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report


def build_balanced_manifest(
    input_manifest: str | Path,
    output: str | Path,
    *,
    max_train_per_label: int = 4_000,
    max_validation_per_label: int = 1_000,
    max_test_per_label: int = 1_000,
    max_segments_per_group: int = 4,
    balance_datasets: bool = False,
    seed: int = 42,
) -> dict[str, Any]:
    """Select a deterministic, group-diverse subset for feature extraction."""

    caps = {
        "train": max_train_per_label,
        "validation": max_validation_per_label,
        "test": max_test_per_label,
    }
    if any(value < 1 for value in caps.values()):
        raise ValueError("Per-label split limits must be positive")
    if max_segments_per_group < 1:
        raise ValueError("max_segments_per_group must be positive")

    input_path = Path(input_manifest).resolve()
    output_path = Path(output).resolve()
    if output_path == input_path:
        raise ValueError("Output manifest must not overwrite the input manifest")

    group_rows: dict[
        tuple[str, str, str, str],
        list[tuple[int, bytes, int, dict[str, str]]],
    ] = {}
    source_count = 0
    with input_path.open("r", encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        fields = set(reader.fieldnames or ())
        if not REQUIRED_FIELDS.issubset(fields):
            missing = sorted(REQUIRED_FIELDS - fields)
            raise ValueError(f"Manifest {input_path} is missing columns: {missing}")

        for source_row in reader:
            source_count += 1
            row = {
                field: (source_row.get(field) or "").strip()
                for field in MANIFEST_FIELDS
            }
            if not row["group_id"]:
                raise ValueError(f"Manifest row {source_count + 1} has no group_id")
            if row["split"] not in caps:
                raise ValueError(
                    f"Manifest row {source_count + 1} has invalid split {row['split']!r}"
                )

            key = (row["head"], row["label"], row["split"], row["group_id"])
            rank = _stable_rank(row, seed)
            fingerprint = _row_fingerprint(row)
            heap = group_rows.setdefault(key, [])
            entry = (-rank, fingerprint, source_count, row)
            if len(heap) < max_segments_per_group:
                heapq.heappush(heap, entry)
            elif rank < -heap[0][0]:
                heapq.heapreplace(heap, entry)

    buckets: dict[
        tuple[str, str, str], list[tuple[bytes, list[dict[str, str]]]]
    ] = {}
    for (head, label, split, group_id), heap in group_rows.items():
        rows = [entry[3] for entry in sorted(heap, key=lambda item: -item[0])]
        buckets.setdefault((head, label, split), []).append(
            (_group_rank(group_id, seed), rows)
        )

    selected: list[dict[str, str]] = []
    sample_counts: Counter[str] = Counter()
    group_counts: Counter[str] = Counter()
    for (head, label, split), groups in sorted(buckets.items()):
        groups.sort(key=lambda item: item[0])
        if balance_datasets:
            by_dataset: dict[
                str, deque[tuple[bytes, list[dict[str, str]]]]
            ] = {}
            for group in groups:
                dataset = group[1][0]["dataset"] or "(unknown)"
                by_dataset.setdefault(dataset, deque()).append(group)
            interleaved: list[tuple[bytes, list[dict[str, str]]]] = []
            dataset_order = sorted(
                by_dataset,
                key=lambda value: hashlib.sha256(
                    f"{seed}:dataset:{value}".encode("utf-8")
                ).digest(),
            )
            while any(by_dataset.values()):
                for dataset in dataset_order:
                    if by_dataset[dataset]:
                        interleaved.append(by_dataset[dataset].popleft())
            groups = interleaved
        cap = caps[split]
        bucket_selected: list[dict[str, str]] = []
        for depth in range(max_segments_per_group):
            for _rank_value, rows in groups:
                if depth < len(rows):
                    bucket_selected.append(rows[depth])
                    if len(bucket_selected) == cap:
                        break
            if len(bucket_selected) == cap:
                break
        selected.extend(bucket_selected)
        rendered_key = f"{head}/{label}/{split}"
        sample_counts[rendered_key] = len(bucket_selected)
        group_counts[rendered_key] = len(
            {row["group_id"] for row in bucket_selected}
        )

    selected.sort(
        key=lambda row: (
            row["path"].casefold(),
            float(row["start_seconds"] or 0.0),
            row["head"],
            row["label"],
            row["split"],
        )
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f"{output_path.name}.tmp")
    try:
        with temporary_path.open("w", encoding="utf-8-sig", newline="") as target:
            writer = csv.DictWriter(target, fieldnames=MANIFEST_FIELDS)
            writer.writeheader()
            writer.writerows(selected)
        os.replace(temporary_path, output_path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise

    report = {
        "manifest": str(output_path),
        "input": str(input_path),
        "source_count": source_count,
        "selected_count": len(selected),
        "limits": {
            **caps,
            "segments_per_group": max_segments_per_group,
            "balance_datasets": balance_datasets,
        },
        "seed": seed,
        "sample_counts": dict(sorted(sample_counts.items())),
        "unique_group_counts": dict(sorted(group_counts.items())),
        "selection_rule": (
            "stable hash; maximize unique groups before selecting another segment "
            "from the same group; optionally round-robin datasets within each label"
        ),
        "output_order": "path then segment start for HDD locality",
    }
    output_path.with_suffix(".report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report
