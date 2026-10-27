"""Strict 16 kHz mono PCM WAV loading for local ASR."""

from __future__ import annotations

from pathlib import Path
import wave

import numpy as np


def load_pcm16_wav(path: str | Path) -> tuple[int, np.ndarray]:
    source = Path(path)
    with wave.open(str(source), "rb") as handle:
        channels = handle.getnchannels()
        sample_width = handle.getsampwidth()
        sample_rate = handle.getframerate()
        frames = handle.readframes(handle.getnframes())
    if channels != 1:
        raise ValueError(f"ASR input must be mono: {source}")
    if sample_width != 2:
        raise ValueError(f"ASR input must be signed PCM16: {source}")
    if sample_rate != 16_000:
        raise ValueError(f"ASR input must be 16 kHz: {source}")
    samples = np.frombuffer(frames, dtype="<i2").copy()
    return sample_rate, samples


def pcm16_to_float32(samples: np.ndarray) -> np.ndarray:
    return samples.astype(np.float32) / 32768.0
