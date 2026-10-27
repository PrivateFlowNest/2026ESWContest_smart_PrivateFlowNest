from __future__ import annotations

import csv
import json
import zipfile

import numpy as np
from scipy.io import wavfile

from homemind_audio.aihub_manifest import build_aihub_manifest, stable_split
from homemind_audio.dataset import load_manifest


def _write_wav(path, seconds=2):
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, 16_000, np.zeros(16_000 * seconds, dtype=np.int16))


def test_stable_split_keeps_group_together():
    first = stable_split("meeting:recording-1", seed=7)
    assert first == stable_split("meeting:recording-1", seed=7)
    assert first in {"train", "validation"}


def test_build_manifest_maps_meeting_metadata_and_loads_segments(tmp_path):
    root = tmp_path / "aihub"
    meeting = root / "464_meeting"
    wav_root = meeting / "extracted" / "training" / "TS2" / "기타녹음"
    _write_wav(wav_root / "remote.wav", seconds=12)
    _write_wav(wav_root / "local.wav", seconds=12)

    labels = meeting / "archives" / "labels" / "TL2.zip"
    labels.parent.mkdir(parents=True)
    with zipfile.ZipFile(labels, "w") as archive:
        archive.writestr(
            "remote.json",
            json.dumps({"metadata": {"communication": "비대면"}}),
        )
        archive.writestr(
            "local.json",
            json.dumps({"metadata": {"communication": "대면"}}),
        )

    output = tmp_path / "pilot.csv"
    report = build_aihub_manifest(root, output, max_per_label=10, seed=1)
    records = load_manifest(output)

    assert report["selected_count"] == 2
    assert {record.label for record in records} == {
        "IN_PERSON_MEETING",
        "ZOOM_SPEAKER",
    }
    assert all(record.start_seconds is not None for record in records)
    assert all(record.group_id for record in records)


def test_manifest_never_splits_one_group_across_train_and_validation(tmp_path):
    root = tmp_path / "aihub"
    dialogue = (
        root
        / "71698_environment_sound"
        / "extracted"
        / "training"
        / "TS_대화음성데이터_01.경제및산업_01.주거지역"
    )
    _write_wav(dialogue / "01_01_PAIR_0001_VN.wav")
    _write_wav(dialogue / "01_01_PAIR_0001_VV.wav")

    output = tmp_path / "pilot.csv"
    build_aihub_manifest(root, output, max_per_label=10)
    with output.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))

    groups: dict[str, set[str]] = {}
    for row in rows:
        groups.setdefault(row["group_id"], set()).add(row["split"])
    assert groups
    assert all(len(splits) == 1 for splits in groups.values())


def test_free_conversation_reassigns_same_speaker_to_one_split(tmp_path):
    root = tmp_path / "aihub"
    for source_split in ("training", "validation"):
        speaker = (
            root
            / "109_free_conversation"
            / "extracted"
            / source_split
            / "source-archive"
            / "speaker-001"
        )
        _write_wav(speaker / f"{source_split}.wav")

    output = tmp_path / "pilot.csv"
    build_aihub_manifest(root, output, max_per_label=10, seed=9)
    records = load_manifest(output)

    assert {record.group_id for record in records} == {
        "109_free_conversation:speaker-001"
    }
    assert len({record.split for record in records}) == 1


def test_full_manifest_streams_all_rows_and_replaces_temporary_file(tmp_path):
    root = tmp_path / "aihub"
    broadcast = root / "463_broadcast" / "extracted" / "training"
    _write_wav(broadcast / "first.wav")
    _write_wav(broadcast / "second.wav")

    output = tmp_path / "full.csv"
    report = build_aihub_manifest(root, output, max_per_label=None)
    records = load_manifest(output)

    assert report["candidate_count"] == 2
    assert report["selected_count"] == 2
    assert len(records) == 2
    assert not output.with_name("full.csv.tmp").exists()
