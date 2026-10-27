from __future__ import annotations

import csv

import numpy as np

from homemind_audio.recording import append_manifest, save_clip


def test_save_clip_and_append_manifest(tmp_path):
    manifest = tmp_path / "manifest.csv"
    clip = save_clip(tmp_path / "raw" / "quiet.wav", np.zeros(16_000))
    append_manifest(
        manifest,
        audio_path=clip,
        head="activity",
        label="QUIET",
        split="train",
    )

    with manifest.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle))

    assert rows == [
        {
            "path": "raw/quiet.wav",
            "head": "activity",
            "label": "QUIET",
            "split": "train",
        }
    ]
