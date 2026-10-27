import wave

import numpy as np
import pytest

from homemind_stt.audio import load_pcm16_wav, pcm16_to_float32


def write_wav(path, *, rate=16000, channels=1):
    samples = np.array([0, 32767, -32768], dtype="<i2")
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(samples.tobytes())


def test_strict_wav_loading(tmp_path):
    path = tmp_path / "test.wav"
    write_wav(path)
    rate, samples = load_pcm16_wav(path)
    assert rate == 16000
    assert samples.tolist() == [0, 32767, -32768]
    assert pcm16_to_float32(samples).dtype == np.float32


def test_non_16k_is_rejected(tmp_path):
    path = tmp_path / "bad.wav"
    write_wav(path, rate=8000)
    with pytest.raises(ValueError, match="16 kHz"):
        load_pcm16_wav(path)
