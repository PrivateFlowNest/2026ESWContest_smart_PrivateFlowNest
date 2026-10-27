"""Whisper (whisper.cpp) transcription of one endpointed utterance.

Reuses ``homemind_stt.backends`` (same whisper-cli flags as the PC STT evaluation, or the
whisper-server HTTP client). The utterance is written to a temporary 16 kHz PCM16 WAV inside a
private temp directory and deleted right after transcription: raw audio never persists.
"""

from __future__ import annotations

import logging
import re
import tempfile
import time
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

import numpy as np

from .vad import SAMPLE_RATE

log = logging.getLogger(__name__)

# Phrases whisper produces from noise/silence (YouTube-subtitle hallucinations).
_HALLUCINATIONS = {
    "감사합니다", "시청해주셔서 감사합니다", "시청해 주셔서 감사합니다", "구독과 좋아요 부탁드립니다",
    "다음 영상에서 만나요", "MBC 뉴스 이덕영입니다", "끝", "자막 제공 및 자막 편집",
}
_BRACKETED = re.compile(r"[\[\(（【][^\]\)）】]*[\]\)）】]")


def clean_transcript(text: str) -> str:
    """Drop whisper markers ([BLANK_AUDIO], (박수) ...) and known silence hallucinations."""

    value = " ".join(line.strip().lstrip("-").strip() for line in text.splitlines() if line.strip())
    value = _BRACKETED.sub(" ", value)
    value = re.sub(r"\s+", " ", value).strip()
    bare = value.strip(" .,!?~…")
    if not bare or bare in _HALLUCINATIONS or not re.search(r"[0-9A-Za-z가-힣]", bare):
        return ""
    return value


@dataclass(frozen=True)
class SttResult:
    text: str
    raw_length: int
    latency_ms: float
    model_version: str


def _make_backend(settings: Mapping[str, Any]):
    from homemind_stt.backends import WhisperCppBackend, WhisperCppHttpBackend

    model_version = str(settings.get("stt_model_version") or "")
    server_url = settings.get("whisper_server_url")
    model_file = Path(str(settings["whisper_model"])).resolve()
    if not model_version:
        model_version = f"whisper.cpp-{model_file.stem}"
    model_version = model_version[:64]
    if server_url:
        return WhisperCppHttpBackend(str(server_url), model_version=model_version)
    executable = Path(str(settings["whisper_cli_path"])).resolve()
    if not executable.is_file():
        raise FileNotFoundError(f"whisper-cli not found: {executable}")
    if not model_file.is_file():
        raise FileNotFoundError(f"whisper model not found: {model_file}")
    # WhisperCppBackend hard-codes <dir>/whisper-cli.exe; set its fields directly so the same
    # transcribe() (flags -l ko -np -nt -ng) works for any executable path (Linux build later).
    backend = WhisperCppBackend.__new__(WhisperCppBackend)
    backend.executable = executable
    backend.model = model_file
    backend.num_threads = int(settings.get("whisper_threads", 4))
    backend.model_version = model_version
    return backend


class WhisperTranscriber:
    def __init__(self, settings: Mapping[str, Any], *, backend: Any | None = None) -> None:
        self.backend = backend if backend is not None else _make_backend(settings)
        self.model_version = str(getattr(self.backend, "model_version", "whisper"))[:64]

    def transcribe(self, samples: np.ndarray) -> SttResult:
        # One private temp dir per utterance, removed with its WAV as soon as whisper returns.
        started = time.perf_counter()
        with tempfile.TemporaryDirectory(prefix="hm-stt-") as folder:
            path = Path(folder) / "utterance.wav"
            try:
                write_pcm16_wav(path, samples)
                transcript = self.backend.transcribe(path)
            finally:
                path.unlink(missing_ok=True)
        latency_ms = (time.perf_counter() - started) * 1000.0
        raw = str(getattr(transcript, "text", transcript))
        return SttResult(clean_transcript(raw), len(raw), latency_ms, self.model_version)

    def close(self) -> None:
        pass


def write_pcm16_wav(path: str | Path, samples: np.ndarray) -> None:
    values = np.clip(np.asarray(samples, dtype=np.float32).reshape(-1), -1.0, 1.0)
    pcm = (values * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(pcm.tobytes())
