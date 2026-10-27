from __future__ import annotations

import csv

from homemind_audio.realised_manifest import build_realised_manifest


def test_realised_is_other_hard_negative_only(tmp_path):
    root = tmp_path / "realised"
    root.mkdir()
    (root / "was_001_00_00_0.flac").write_bytes(b"test")
    (root / "tel_001_00_00_0.flac").write_bytes(b"test")
    (root / "unknown.flac").write_bytes(b"test")
    output = tmp_path / "realised.csv"

    report = build_realised_manifest(root, output)
    with output.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))

    assert len(rows) == 2
    assert {row["label"] for row in rows} == {"OTHER"}
    assert {row["head"] for row in rows} == {"sound_event"}
    assert report["ignored_files"] == 1
    assert "not treated as WASHER_COMPLETE" in report["label_policy"]
