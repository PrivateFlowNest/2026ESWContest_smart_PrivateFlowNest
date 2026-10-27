from __future__ import annotations

import csv
import json

import numpy as np
from scipy.io import wavfile

from homemind_audio.fsd50k_manifest import (
    build_fsd50k_manifest,
    map_fsd50k_label,
)


def _write_wav(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, 44_100, np.zeros(44_100, dtype=np.int16))


def test_label_mapping_prioritizes_doorbell_over_alarm_parent():
    assert map_fsd50k_label("Alarm,Doorbell") == "DOORBELL"
    assert map_fsd50k_label("Alarm,Siren") == "ALARM"
    assert map_fsd50k_label("Bell,Chime") == "OTHER"
    assert map_fsd50k_label("Dog,Bark") is None


def test_build_fsd50k_manifest_preserves_eval_as_test_and_license(tmp_path):
    root = tmp_path / "FSD50K"
    truth = root / "FSD50K.ground_truth"
    truth.mkdir(parents=True)
    with (truth / "dev.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["fname", "labels", "mids", "split"])
        writer.writerow(["100", "Alarm,Doorbell", "mids", "train"])
    with (truth / "eval.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["fname", "labels", "mids"])
        writer.writerow(["200", "Alarm,Siren", "mids"])
    _write_wav(root / "FSD50K.dev_audio" / "100.wav")
    _write_wav(root / "FSD50K.eval_audio" / "200.wav")
    metadata = root / "FSD50K.metadata"
    metadata.mkdir()
    (metadata / "dev_clips_info_FSD50K.json").write_text(
        json.dumps({"100": {"license": "CC-BY", "uploader": "author-1"}}),
        encoding="utf-8",
    )
    (metadata / "eval_clips_info_FSD50K.json").write_text(
        json.dumps({"200": {"license": "CC0", "uploader": "author-2"}}),
        encoding="utf-8",
    )

    output = tmp_path / "fsd50k.csv"
    report = build_fsd50k_manifest(root, output, max_per_label_per_split=10)
    with output.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))

    assert report["selected_count"] == 2
    assert {(row["label"], row["split"]) for row in rows} == {
        ("DOORBELL", "train"),
        ("ALARM", "test"),
    }
    assert next(row for row in rows if row["label"] == "DOORBELL")["license"] == "CC-BY"
    assert next(row for row in rows if row["label"] == "DOORBELL")["uploader"] == "author-1"
