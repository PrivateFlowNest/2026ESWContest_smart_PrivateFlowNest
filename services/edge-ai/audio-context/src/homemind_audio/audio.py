"""WAV loading and normalization utilities for YAMNet."""

from __future__ import annotations

from math import gcd
from pathlib import Path
import wave

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

try:
    import soundfile as sf
except ImportError:  # pragma: no cover - dependency error is reported at use time
    sf = None

TARGET_SAMPLE_RATE = 16_000
MIN_SAMPLES = TARGET_SAMPLE_RATE


def _run_lengths(values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if values.size == 0:
        return np.asarray([], dtype=bool), np.asarray([], dtype=np.int32)
    changes = np.flatnonzero(np.diff(values.astype(np.int8))) + 1
    boundaries = np.concatenate(([0], changes, [values.size]))
    return values[boundaries[:-1]], np.diff(boundaries)


def vad_temporal_features(
    waveform: np.ndarray,
    *,
    sample_rate: int = TARGET_SAMPLE_RATE,
) -> np.ndarray:
    """Return compact energy-VAD and speech/pause duration features."""

    values = np.asarray(waveform, dtype=np.float32).reshape(-1)
    frame_size = max(1, int(round(0.030 * sample_rate)))
    hop_size = max(1, int(round(0.010 * sample_rate)))
    if values.size < frame_size:
        values = np.pad(values, (0, frame_size - values.size))
    frame_count = 1 + (values.size - frame_size) // hop_size
    shape = (frame_count, frame_size)
    strides = (values.strides[0] * hop_size, values.strides[0])
    frames = np.lib.stride_tricks.as_strided(values, shape=shape, strides=strides)
    rms = np.sqrt(np.mean(np.square(frames), axis=1) + 1e-12)
    db = 20.0 * np.log10(rms + 1e-8)
    noise_floor = float(np.percentile(db, 20))
    threshold = max(noise_floor + 8.0, -45.0)
    voiced = db >= threshold
    run_values, run_frames = _run_lengths(voiced)
    frame_seconds = hop_size / sample_rate
    speech_runs = run_frames[run_values] * frame_seconds
    pause_runs = run_frames[~run_values] * frame_seconds

    def statistics(runs: np.ndarray) -> tuple[float, float, float]:
        if runs.size == 0:
            return 0.0, 0.0, 0.0
        return float(runs.mean()), float(runs.std()), float(runs.max())

    speech_mean, speech_std, speech_max = statistics(speech_runs)
    pause_mean, pause_std, pause_max = statistics(pause_runs)
    return np.asarray(
        [
            float(voiced.mean()),
            float(speech_runs.size / max(frame_count, 1)),
            speech_mean,
            speech_std,
            speech_max,
            pause_mean,
            pause_std,
            pause_max,
            float(np.mean(db) / 100.0),
            float(np.std(db) / 100.0),
            float((np.percentile(db, 90) - np.percentile(db, 10)) / 100.0),
            float(noise_floor / 100.0),
        ],
        dtype=np.float32,
    )


def _to_float32(samples: np.ndarray) -> np.ndarray:
    if np.issubdtype(samples.dtype, np.floating):
        return np.clip(samples.astype(np.float32), -1.0, 1.0)

    if samples.dtype == np.uint8:
        return (samples.astype(np.float32) - 128.0) / 128.0

    if np.issubdtype(samples.dtype, np.signedinteger):
        limit = float(max(abs(np.iinfo(samples.dtype).min), np.iinfo(samples.dtype).max))
        return samples.astype(np.float32) / limit

    raise ValueError(f"Unsupported WAV sample dtype: {samples.dtype}")


def load_wav_16k_mono(path: str | Path) -> np.ndarray:
    """Load a WAV or FLAC file as mono, 16 kHz, float32 samples in [-1, 1]."""

    audio_path = Path(path)
    if audio_path.suffix.casefold() == ".flac":
        if sf is None:
            raise RuntimeError("FLAC input requires the soundfile package")
        samples, sample_rate = sf.read(audio_path, dtype="float32", always_2d=False)
        samples = np.asarray(samples, dtype=np.float32)
    else:
        sample_rate, samples = wavfile.read(audio_path)
        samples = _to_float32(np.asarray(samples))

    if samples.ndim == 2:
        samples = samples.mean(axis=1)
    elif samples.ndim != 1:
        raise ValueError(f"Expected mono or stereo WAV, got shape {samples.shape}")

    if sample_rate != TARGET_SAMPLE_RATE:
        divisor = gcd(int(sample_rate), TARGET_SAMPLE_RATE)
        samples = resample_poly(
            samples,
            TARGET_SAMPLE_RATE // divisor,
            int(sample_rate) // divisor,
        ).astype(np.float32)

    if samples.size < MIN_SAMPLES:
        samples = np.pad(samples, (0, MIN_SAMPLES - samples.size))

    return np.clip(samples, -1.0, 1.0).astype(np.float32, copy=False)


def load_wav_segment_16k_mono(
    path: str | Path,
    start_seconds: float,
    end_seconds: float,
) -> np.ndarray:
    """Read one WAV/FLAC time range and normalize it for YAMNet."""

    if start_seconds < 0 or end_seconds <= start_seconds:
        raise ValueError("Invalid WAV segment bounds")

    audio_path = Path(path)
    if audio_path.suffix.casefold() == ".flac":
        if sf is None:
            raise RuntimeError("FLAC input requires the soundfile package")
        info = sf.info(audio_path)
        sample_rate = info.samplerate
        start = max(0, int(round(start_seconds * sample_rate)))
        end = min(info.frames, int(round(end_seconds * sample_rate)))
        if end <= start:
            raise ValueError(
                f"Segment {start_seconds:.3f}-{end_seconds:.3f}s is outside {path}"
            )
        segment, _ = sf.read(
            audio_path,
            start=start,
            stop=end,
            dtype="float32",
            always_2d=False,
        )
        segment = np.asarray(segment, dtype=np.float32)
    else:
        try:
            with wave.open(str(path), "rb") as handle:
                sample_rate = handle.getframerate()
                channels = handle.getnchannels()
                sample_width = handle.getsampwidth()
                frame_count = handle.getnframes()
                start = max(0, int(round(start_seconds * sample_rate)))
                end = min(frame_count, int(round(end_seconds * sample_rate)))
                if end <= start:
                    raise ValueError(
                        f"Segment {start_seconds:.3f}-{end_seconds:.3f}s is outside {path}"
                    )
                handle.setpos(start)
                raw = handle.readframes(end - start)
            if sample_width == 1:
                segment = _to_float32(np.frombuffer(raw, dtype=np.uint8))
            elif sample_width == 2:
                segment = _to_float32(np.frombuffer(raw, dtype="<i2"))
            elif sample_width == 3:
                octets = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
                values = (
                    octets[:, 0].astype(np.int32)
                    | (octets[:, 1].astype(np.int32) << 8)
                    | (octets[:, 2].astype(np.int32) << 16)
                )
                values = np.where(values & 0x800000, values - 0x1000000, values)
                segment = values.astype(np.float32) / 8_388_608.0
            elif sample_width == 4:
                segment = _to_float32(np.frombuffer(raw, dtype="<i4"))
            else:
                raise ValueError(f"Unsupported PCM sample width: {sample_width}")
            if channels > 1:
                segment = segment.reshape(-1, channels)
        except wave.Error:
            # Rare IEEE-float WAVs are not supported by the standard wave module.
            sample_rate, samples = wavfile.read(Path(path))
            start = max(0, int(round(start_seconds * sample_rate)))
            end = min(len(samples), int(round(end_seconds * sample_rate)))
            if end <= start:
                raise ValueError(
                    f"Segment {start_seconds:.3f}-{end_seconds:.3f}s is outside {path}"
                )
            segment = _to_float32(np.asarray(samples[start:end]))

    if segment.ndim == 2:
        segment = segment.mean(axis=1)
    elif segment.ndim != 1:
        raise ValueError(f"Expected mono or stereo WAV, got shape {segment.shape}")

    if sample_rate != TARGET_SAMPLE_RATE:
        divisor = gcd(int(sample_rate), TARGET_SAMPLE_RATE)
        segment = resample_poly(
            segment,
            TARGET_SAMPLE_RATE // divisor,
            int(sample_rate) // divisor,
        ).astype(np.float32)
    if segment.size < MIN_SAMPLES:
        segment = np.pad(segment, (0, MIN_SAMPLES - segment.size))
    return np.clip(segment, -1.0, 1.0).astype(np.float32, copy=False)
