import json
import wave
import zipfile

import numpy as np
import pytest

from homemind_stt.dataset import build_aihub_manifest


def test_manifest_matches_speech_mix_without_leaking_audio(tmp_path):
    audio_root = tmp_path / "audio"
    audio_root.mkdir()
    wav = audio_root / "sample-S.wav"
    with wave.open(str(wav), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(16000)
        handle.writeframes(np.zeros(160, dtype="<i2").tobytes())

    labels = tmp_path / "labels.zip"
    payload = {
        "file": {"name": "sample", "length": 0.01},
        "command": {"text": "거실 불을 켜줘", "domain": "가정", "snr": "12db"},
    }
    with zipfile.ZipFile(labels, "w") as archive:
        archive.writestr("sample-J.json", json.dumps(payload, ensure_ascii=False))

    rows = build_aihub_manifest(labels, audio_root, limit=1, keywords=("불",))
    assert len(rows) == 1
    assert rows[0]["audio_path"].endswith("sample-S.wav")
    assert rows[0]["privacy_scope"] == "LOCAL_EVALUATION_ONLY"
    assert rows[0]["audio_variant"] == "S"


def test_manifest_rejects_non_positive_limit(tmp_path):
    with pytest.raises(ValueError, match="at least 1"):
        build_aihub_manifest(tmp_path / "labels.zip", tmp_path, limit=-1)
