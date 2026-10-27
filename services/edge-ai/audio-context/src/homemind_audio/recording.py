"""Local microphone data collection for the PC baseline."""

from __future__ import annotations

import csv
from math import gcd
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

from .audio import TARGET_SAMPLE_RATE
from .labels import HEAD_LABELS


def record_clip(seconds: float, *, device: int | str | None = None) -> np.ndarray:
    if seconds <= 0:
        raise ValueError("seconds must be positive")

    try:
        import sounddevice as sound_device
    except ImportError as exc:
        raise RuntimeError("sounddevice is not installed") from exc

    recording_rate = TARGET_SAMPLE_RATE
    try:
        sound_device.check_input_settings(
            device=device,
            channels=1,
            dtype="float32",
            samplerate=TARGET_SAMPLE_RATE,
        )
    except sound_device.PortAudioError:
        recording_rate = int(
            round(sound_device.query_devices(device, "input")["default_samplerate"])
        )

    frames = int(round(seconds * recording_rate))
    audio = sound_device.rec(
        frames,
        samplerate=recording_rate,
        channels=1,
        dtype="float32",
        device=device,
    )
    sound_device.wait()
    waveform = audio[:, 0]
    if recording_rate != TARGET_SAMPLE_RATE:
        divisor = gcd(recording_rate, TARGET_SAMPLE_RATE)
        waveform = resample_poly(
            waveform,
            TARGET_SAMPLE_RATE // divisor,
            recording_rate // divisor,
        )
    return np.clip(waveform, -1.0, 1.0).astype(np.float32, copy=False)


def list_input_devices() -> list[dict[str, object]]:
    try:
        import sounddevice as sound_device
    except ImportError as exc:
        raise RuntimeError("sounddevice is not installed") from exc

    default_input = sound_device.default.device[0]
    devices: list[dict[str, object]] = []
    for index, info in enumerate(sound_device.query_devices()):
        if int(info["max_input_channels"]) <= 0:
            continue
        devices.append(
            {
                "index": index,
                "name": str(info["name"]),
                "channels": int(info["max_input_channels"]),
                "default_sample_rate": int(info["default_samplerate"]),
                "default": index == default_input,
            }
        )
    return devices


def save_clip(path: str | Path, waveform: np.ndarray) -> Path:
    output_path = Path(path).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    pcm = (np.clip(waveform, -1.0, 1.0) * np.iinfo(np.int16).max).astype(np.int16)
    wavfile.write(output_path, TARGET_SAMPLE_RATE, pcm)
    return output_path


def append_manifest(
    manifest_path: str | Path,
    *,
    audio_path: str | Path,
    head: str,
    label: str,
    split: str,
) -> None:
    normalized_label = label.upper()
    normalized_split = split.lower()
    if head not in HEAD_LABELS:
        raise ValueError(f"Unknown head: {head}")
    if normalized_label not in HEAD_LABELS[head]:
        raise ValueError(f"Invalid label {normalized_label!r} for {head!r}")
    if normalized_split not in {"train", "validation", "test"}:
        raise ValueError(f"Invalid split: {normalized_split}")

    manifest = Path(manifest_path).resolve()
    manifest.parent.mkdir(parents=True, exist_ok=True)
    write_header = not manifest.exists() or manifest.stat().st_size == 0
    relative_audio = Path(audio_path).resolve().relative_to(manifest.parent)

    with manifest.open("a", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        if write_header:
            writer.writerow(["path", "head", "label", "split"])
        writer.writerow(
            [relative_audio.as_posix(), head, normalized_label, normalized_split]
        )
