from __future__ import annotations

import numpy as np
import pytest
from scipy.io import wavfile

from homemind_audio.audio import (
    TARGET_SAMPLE_RATE,
    load_wav_16k_mono,
    load_wav_segment_16k_mono,
    vad_temporal_features,
)


def test_load_wav_converts_stereo_8k_to_mono_16k(tmp_path):
    sample_rate = 8_000
    time = np.arange(sample_rate // 2) / sample_rate
    tone = (np.sin(2 * np.pi * 440 * time) * 20_000).astype(np.int16)
    stereo = np.column_stack([tone, tone])
    wav_path = tmp_path / "tone.wav"
    wavfile.write(wav_path, sample_rate, stereo)

    waveform = load_wav_16k_mono(wav_path)

    assert waveform.dtype == np.float32
    assert waveform.shape == (TARGET_SAMPLE_RATE,)
    assert np.max(np.abs(waveform)) <= 1.0


def test_load_wav_segment_reads_requested_range(tmp_path):
    sample_rate = 16_000
    samples = np.concatenate(
        [
            np.zeros(sample_rate, dtype=np.int16),
            np.full(sample_rate, 10_000, dtype=np.int16),
        ]
    )
    wav_path = tmp_path / "two-seconds.wav"
    wavfile.write(wav_path, sample_rate, samples)

    waveform = load_wav_segment_16k_mono(wav_path, 1.0, 2.0)

    assert waveform.shape == (sample_rate,)
    assert float(waveform.mean()) > 0.25


def test_load_flac_and_segment(tmp_path):
    soundfile = pytest.importorskip("soundfile")
    sample_rate = 44_100
    samples = np.concatenate(
        [
            np.zeros(sample_rate, dtype=np.float32),
            np.full(sample_rate, 0.5, dtype=np.float32),
        ]
    )
    flac_path = tmp_path / "two-seconds.flac"
    soundfile.write(flac_path, samples, sample_rate)

    complete = load_wav_16k_mono(flac_path)
    segment = load_wav_segment_16k_mono(flac_path, 1.0, 2.0)

    assert complete.shape == (TARGET_SAMPLE_RATE * 2,)
    assert segment.shape == (TARGET_SAMPLE_RATE,)
    assert float(segment.mean()) > 0.45


def test_vad_features_distinguish_silence_from_alternating_speech():
    sample_rate = 16_000
    silence = np.zeros(sample_rate * 4, dtype=np.float32)
    tone = np.sin(2 * np.pi * 220 * np.arange(sample_rate) / sample_rate).astype(
        np.float32
    )
    alternating = np.concatenate([tone, np.zeros_like(tone), tone, np.zeros_like(tone)])

    quiet_features = vad_temporal_features(silence)
    alternating_features = vad_temporal_features(alternating)

    assert quiet_features.shape == (12,)
    assert alternating_features[0] > quiet_features[0]
    assert alternating_features[5] > 0
