from __future__ import annotations

import csv

import numpy as np
from scipy.io import wavfile

from homemind_audio.tau_manifest import build_tau_manifest


def _wav(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, 16_000, np.zeros(16_000, dtype=np.int16))


def test_tau_manifest_reuses_fsd_groups_and_excludes_eval(tmp_path):
    root = tmp_path / "tau"
    _wav(root / "final_data_wav_split" / "train" / "100_mix.wav")
    _wav(root / "final_data_wav_split" / "val" / "200_mix.wav")
    _wav(root / "final_data_wav_split" / "eval" / "300_mix.wav")
    metadata = root / "metadata.csv"
    with metadata.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "split",
                "file_name",
                "event_class",
                "speech_present",
                "original_freesound_file",
                "original_librispeech_file",
                "speaker_id",
                "speaker_gender",
            ]
        )
        writer.writerow(["dev", "100_mix.npy", "doorbell", "speech", "100_1.npy", "", "", ""])
        writer.writerow(["dev", "200_mix.npy", "doorbell", "speech", "200_1.npy", "", "", ""])
        writer.writerow(["eval", "300_mix.npy", "doorbell", "speech", "300_1.npy", "", "", ""])
    reference = tmp_path / "reference.csv"
    with reference.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["group_id", "split"])
        writer.writeheader()
        writer.writerow({"group_id": "fsd50k:100", "split": "validation"})
        writer.writerow({"group_id": "fsd50k:200", "split": "train"})
        writer.writerow({"group_id": "fsd50k:300", "split": "test"})

    output = tmp_path / "tau.csv"
    report = build_tau_manifest(
        root, output, metadata=metadata, reference_manifest=reference
    )
    with output.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))

    assert {(row["group_id"], row["split"]) for row in rows} == {
        ("fsd50k:100", "validation"),
        ("fsd50k:200", "train"),
    }
    assert all(row["label"] == "DOORBELL" for row in rows)
    assert report["eval_or_test_source_rows_excluded"] == 1
    assert report["reference_split_overrides"] == 2


def test_tau_manifest_only_adds_other_when_explicit(tmp_path):
    root = tmp_path / "tau"
    _wav(root / "train" / "400_mix.wav")
    metadata = root / "metadata.csv"
    with metadata.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["split", "file_name", "event_class", "original_freesound_file"])
        writer.writerow(["dev", "400_mix.npy", "dog_barking", "400_1.npy"])

    output = tmp_path / "tau.csv"
    report = build_tau_manifest(root, output, metadata=metadata)
    assert report["selected_count"] == 0

    report = build_tau_manifest(root, output, metadata=metadata, include_other=True)
    assert report["selected_count"] == 1


def test_tau_manifest_keeps_unreferenced_source_group_in_one_split(tmp_path):
    root = tmp_path / "tau"
    _wav(root / "train" / "500_part1.wav")
    _wav(root / "val" / "500_part2.wav")
    metadata = root / "metadata.csv"
    with metadata.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["split", "file_name", "event_class", "original_freesound_file"])
        writer.writerow(["dev", "500_part1.npy", "doorbell", "500_1.npy"])
        writer.writerow(["dev", "500_part2.npy", "doorbell", "500_2.npy"])

    output = tmp_path / "tau.csv"
    build_tau_manifest(root, output, metadata=metadata)
    with output.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))

    assert {row["group_id"] for row in rows} == {"fsd50k:500"}
    assert len({row["split"] for row in rows}) == 1
