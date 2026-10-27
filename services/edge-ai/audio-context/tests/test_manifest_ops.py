from __future__ import annotations

import csv

from homemind_audio.manifest_ops import (
    MANIFEST_FIELDS,
    build_balanced_manifest,
    merge_manifests,
    reserve_validation_groups,
    windowize_manifest,
)


def _write(path, fieldnames, rows):
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def test_merge_manifests_preserves_union_metadata_and_skips_exact_duplicates(
    tmp_path,
):
    common = {
        "path": "sample.wav",
        "head": "sound_event",
        "label": "ALARM",
        "split": "train",
        "start_seconds": "0.000",
        "end_seconds": "5.000",
        "group_id": "source:1",
        "dataset": "AIHUB",
        "source": "source one",
    }
    first = tmp_path / "first.csv"
    second = tmp_path / "second.csv"
    output = tmp_path / "merged.csv"
    _write(first, tuple(common), [common])
    fsd_row = {
        **common,
        "path": "doorbell.wav",
        "label": "DOORBELL",
        "group_id": "fsd50k:2",
        "dataset": "FSD50K",
        "license": "CC0",
        "uploader": "tester",
        "source_url": "https://example.test/2",
    }
    _write(second, MANIFEST_FIELDS, [common, fsd_row])

    report = merge_manifests([first, second], output)

    with output.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    assert len(rows) == 2
    assert rows[1]["license"] == "CC0"
    assert report["selected_count"] == 2
    assert report["exact_duplicate_rows_skipped"] == 1
    assert not output.with_name("merged.csv.tmp").exists()


def test_balanced_manifest_maximizes_groups_before_repeated_segments(tmp_path):
    source = tmp_path / "source.csv"
    output = tmp_path / "balanced.csv"
    rows = []
    for group_id in ("group-a", "group-b", "group-c"):
        for segment in range(3):
            rows.append(
                {
                    "path": f"{group_id}.wav",
                    "head": "activity",
                    "label": "SELF_TALK",
                    "split": "train",
                    "start_seconds": str(segment),
                    "end_seconds": str(segment + 1),
                    "group_id": group_id,
                    "dataset": "test",
                    "source": "fixture",
                }
            )
    _write(source, MANIFEST_FIELDS, rows)

    report = build_balanced_manifest(
        source,
        output,
        max_train_per_label=4,
        max_validation_per_label=1,
        max_test_per_label=1,
        max_segments_per_group=2,
        seed=7,
    )

    with output.open(encoding="utf-8-sig", newline="") as handle:
        selected = list(csv.DictReader(handle))
    assert len(selected) == 4
    assert len({row["group_id"] for row in selected}) == 3
    assert report["sample_counts"]["activity/SELF_TALK/train"] == 4
    assert report["unique_group_counts"]["activity/SELF_TALK/train"] == 3


def test_balanced_manifest_round_robins_datasets_before_filling_cap(tmp_path):
    source = tmp_path / "source.csv"
    output = tmp_path / "balanced.csv"
    rows = []
    for dataset, count in (("large", 10), ("small", 2)):
        for index in range(count):
            rows.append(
                {
                    "path": f"{dataset}-{index}.wav",
                    "head": "sound_event",
                    "label": "OTHER",
                    "split": "train",
                    "start_seconds": "0",
                    "end_seconds": "1",
                    "group_id": f"{dataset}-{index}",
                    "dataset": dataset,
                    "source": "fixture",
                }
            )
    _write(source, MANIFEST_FIELDS, rows)

    report = build_balanced_manifest(
        source,
        output,
        max_train_per_label=6,
        max_validation_per_label=1,
        max_test_per_label=1,
        max_segments_per_group=1,
        balance_datasets=True,
        seed=7,
    )

    with output.open(encoding="utf-8-sig", newline="") as handle:
        selected = list(csv.DictReader(handle))
    counts = {name: sum(row["dataset"] == name for row in selected) for name in ("large", "small")}
    assert counts == {"large": 4, "small": 2}
    assert report["limits"]["balance_datasets"] is True
    assert [row["path"] for row in selected] == sorted(row["path"] for row in selected)


def test_windowize_manifest_matches_runtime_window_and_preserves_split(tmp_path):
    source = tmp_path / "source.csv"
    output = tmp_path / "windowed.csv"
    rows = [
        {
            "path": "meeting.wav",
            "head": "activity",
            "label": "IN_PERSON_MEETING",
            "split": "validation",
            "start_seconds": "10",
            "end_seconds": "18",
            "group_id": "meeting-1",
            "dataset": "test",
        },
        {
            "path": "short.wav",
            "head": "activity",
            "label": "SELF_TALK",
            "split": "train",
            "start_seconds": "0",
            "end_seconds": "3.5",
            "group_id": "self-1",
            "dataset": "test",
        },
        {
            "path": "bell.wav",
            "head": "sound_event",
            "label": "DOORBELL",
            "split": "test",
            "start_seconds": "",
            "end_seconds": "",
            "group_id": "bell-1",
            "dataset": "test",
        },
    ]
    _write(source, MANIFEST_FIELDS, rows)

    report = windowize_manifest(
        source,
        output,
        head="activity",
        window_seconds=4.0,
        hop_seconds=2.0,
        short_policy="drop",
    )

    with output.open(encoding="utf-8-sig", newline="") as handle:
        selected = list(csv.DictReader(handle))
    activity = [row for row in selected if row["head"] == "activity"]
    assert [(row["start_seconds"], row["end_seconds"]) for row in activity] == [
        ("10.000", "14.000"),
        ("12.000", "16.000"),
        ("14.000", "18.000"),
    ]
    assert {row["split"] for row in activity} == {"validation"}
    assert {row["group_id"] for row in activity} == {"meeting-1"}
    assert selected[-1]["head"] == "sound_event"
    assert report["short_rows_dropped"] == 1
    assert report["target_windows"] == 3


def test_windowize_manifest_rejects_hop_larger_than_window(tmp_path):
    source = tmp_path / "source.csv"
    _write(source, MANIFEST_FIELDS, [])

    try:
        windowize_manifest(source, tmp_path / "output.csv", hop_seconds=5.0)
    except ValueError as exc:
        assert "must not exceed" in str(exc)
    else:
        raise AssertionError("expected an invalid window/hop configuration to fail")


def test_reserve_validation_groups_moves_whole_groups_to_test(tmp_path):
    source = tmp_path / "source.csv"
    output = tmp_path / "split.csv"
    rows = []
    for label in ("IN_PERSON_MEETING", "TV_MEDIA"):
        for group_index in range(3):
            for segment in range(2):
                rows.append(
                    {
                        "path": f"{label}-{group_index}.wav",
                        "head": "activity",
                        "label": label,
                        "split": "validation",
                        "start_seconds": str(segment * 4),
                        "end_seconds": str(segment * 4 + 4),
                        "group_id": f"{label}-{group_index}",
                    }
                )
    _write(source, MANIFEST_FIELDS, rows)

    report = reserve_validation_groups(
        source,
        output,
        groups_per_label=1,
        seed=7,
    )

    with output.open(encoding="utf-8-sig", newline="") as handle:
        selected = list(csv.DictReader(handle))
    for label in ("IN_PERSON_MEETING", "TV_MEDIA"):
        test_groups = {
            row["group_id"]
            for row in selected
            if row["label"] == label and row["split"] == "test"
        }
        validation_groups = {
            row["group_id"]
            for row in selected
            if row["label"] == label and row["split"] == "validation"
        }
        assert len(test_groups) == 1
        assert test_groups.isdisjoint(validation_groups)
        assert sum(
            row["split"] == "test" and row["label"] == label for row in selected
        ) == 2
    assert report["group_overlap_count"] == 0
