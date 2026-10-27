from __future__ import annotations

import csv

from pathlib import Path

from homemind_audio.dataset import manifest_audio_path
from homemind_audio.readiness import FINAL_REQUIRED_LABELS, assess_final_readiness


def _write_manifest(path, rows):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split", "group_id"])
        writer.writerows(rows)


def test_incomplete_manifest_reports_missing_final_classes(tmp_path):
    wav = tmp_path / "sample.wav"
    wav.touch()
    manifest = tmp_path / "manifest.csv"
    _write_manifest(
        manifest,
        [[wav.name, "activity", "SELF_TALK", "train", "speaker-1"]],
    )

    report = assess_final_readiness(
        manifest,
        minimum_train_per_label=1,
        minimum_validation_per_label=1,
        minimum_test_per_label=1,
    )

    assert report["ready_for_final_training"] is False
    assert "required_label_coverage" in report["blockers"]
    assert any(gap["label"] == "WASHER_COMPLETE" for gap in report["coverage_gaps"])


def test_complete_manifest_passes_structural_gate(tmp_path):
    rows = []
    index = 0
    for head, labels in FINAL_REQUIRED_LABELS.items():
        for label in labels:
            for split in ("train", "validation", "test"):
                index += 1
                wav = tmp_path / f"sample-{index}.wav"
                wav.touch()
                rows.append([wav.name, head, label, split, f"group-{index}"])
    manifest = tmp_path / "manifest.csv"
    _write_manifest(manifest, rows)

    report = assess_final_readiness(
        manifest,
        minimum_train_per_label=1,
        minimum_validation_per_label=1,
        minimum_test_per_label=1,
    )

    assert report["ready_for_final_training"] is True
    assert report["blockers"] == []


def test_readiness_counts_unique_original_groups_not_segments(tmp_path):
    wav = tmp_path / "sample.wav"
    wav.touch()
    manifest = tmp_path / "manifest.csv"
    _write_manifest(
        manifest,
        [
            [wav.name, "activity", "SELF_TALK", "train", "speaker-1"],
            [wav.name, "activity", "SELF_TALK", "train", "speaker-1"],
        ],
    )

    report = assess_final_readiness(
        manifest,
        minimum_train_per_label=2,
        minimum_validation_per_label=1,
        minimum_test_per_label=1,
    )

    assert report["sample_counts"]["activity/SELF_TALK/train"] == 2
    assert report["counts"]["activity/SELF_TALK/train"] == 1
    assert any(
        gap["label"] == "SELF_TALK" and gap["split"] == "train"
        for gap in report["coverage_gaps"]
    )


def test_readiness_can_skip_previously_verified_file_paths(tmp_path):
    manifest = tmp_path / "manifest.csv"
    _write_manifest(
        manifest,
        [["missing.wav", "activity", "SELF_TALK", "train", "speaker-1"]],
    )

    report = assess_final_readiness(
        manifest,
        minimum_train_per_label=1,
        minimum_validation_per_label=1,
        minimum_test_per_label=1,
        check_files=False,
    )

    assert report["record_count"] == 1
    assert report["file_existence_checked"] is False


def test_windows_manifest_path_maps_to_wsl_mount():
    mapped = manifest_audio_path(
        r"E:\HomeMind-Datasets\sample.wav",
        Path("/workspace/data"),
        platform_name="posix",
        windows_mount_root=Path("/mnt"),
    )

    assert mapped == Path("/mnt/e/HomeMind-Datasets/sample.wav")
