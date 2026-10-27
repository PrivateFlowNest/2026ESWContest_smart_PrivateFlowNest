"""Lazy local ASR backends with one common transcription interface."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import subprocess
from typing import Iterator
from uuid import uuid4

from .audio import load_pcm16_wav, pcm16_to_float32
from .events import transcript_event


@dataclass(frozen=True)
class Transcript:
    text: str
    model_version: str
    partials: tuple[str, ...] = ()


class VoskBackend:
    def __init__(self, model_dir: str | Path, *, chunk_ms: int = 250) -> None:
        try:
            from vosk import Model, SetLogLevel
        except ImportError as exc:
            raise RuntimeError("install the 'vosk' optional dependency") from exc
        SetLogLevel(-1)
        self.model_dir = Path(model_dir)
        self.model = Model(str(self.model_dir))
        self.chunk_ms = chunk_ms
        self.model_version = self.model_dir.name

    def transcribe(self, wav_path: str | Path) -> Transcript:
        from vosk import KaldiRecognizer

        sample_rate, samples = load_pcm16_wav(wav_path)
        recognizer = KaldiRecognizer(self.model, sample_rate)
        chunk_samples = max(1, sample_rate * self.chunk_ms // 1000)
        finalized: list[str] = []
        partials: list[str] = []
        last_partial = ""
        for offset in range(0, len(samples), chunk_samples):
            data = samples[offset : offset + chunk_samples].astype("<i2", copy=False).tobytes()
            if recognizer.AcceptWaveform(data):
                text = json.loads(recognizer.Result()).get("text", "").strip()
                if text:
                    finalized.append(text)
            else:
                text = json.loads(recognizer.PartialResult()).get("partial", "").strip()
                if text and text != last_partial:
                    partials.append(text)
                    last_partial = text
        text = json.loads(recognizer.FinalResult()).get("text", "").strip()
        if text:
            finalized.append(text)
        return Transcript(" ".join(finalized).strip(), self.model_version, tuple(partials))

    def events(self, wav_path: str | Path) -> Iterator[dict[str, object]]:
        result = self.transcribe(wav_path)
        utterance_id = f"utt-{uuid4()}"
        sequence = 0
        for partial in result.partials:
            yield transcript_event(
                partial, utterance_id=utterance_id, sequence=sequence,
                is_final=False, model_version=result.model_version,
            )
            sequence += 1
        yield transcript_event(
            result.text, utterance_id=utterance_id, sequence=sequence,
            is_final=True, model_version=result.model_version,
        )


class SherpaOfflineBackend:
    def __init__(self, model_dir: str | Path, *, num_threads: int = 4) -> None:
        try:
            import sherpa_onnx
        except ImportError as exc:
            raise RuntimeError("install the 'sherpa' optional dependency") from exc
        root = Path(model_dir)
        tokens = root / "tokens.txt"
        encoder_candidates = sorted(root.glob("encoder*.int8.onnx")) or sorted(root.glob("encoder*.onnx"))
        decoder_candidates = sorted(root.glob("decoder*.onnx"))
        joiner_candidates = sorted(root.glob("joiner*.int8.onnx")) or sorted(root.glob("joiner*.onnx"))
        if not tokens.is_file() or not encoder_candidates or not decoder_candidates or not joiner_candidates:
            raise FileNotFoundError(f"incomplete sherpa-onnx transducer model: {root}")
        self.recognizer = sherpa_onnx.OfflineRecognizer.from_transducer(
            tokens=str(tokens), encoder=str(encoder_candidates[0]),
            decoder=str(decoder_candidates[0]), joiner=str(joiner_candidates[0]),
            num_threads=num_threads, sample_rate=16000, feature_dim=80,
            decoding_method="greedy_search",
        )
        quantization = "int8" if ".int8." in encoder_candidates[0].name else "fp32"
        self.model_version = f"{root.name}-{quantization}"

    def transcribe(self, wav_path: str | Path) -> Transcript:
        sample_rate, samples = load_pcm16_wav(wav_path)
        stream = self.recognizer.create_stream()
        stream.accept_waveform(sample_rate, pcm16_to_float32(samples))
        self.recognizer.decode_stream(stream)
        return Transcript(stream.result.text.strip(), self.model_version)


class WhisperCppBackend:
    """Multilingual whisper.cpp CPU baseline used for PC candidate selection."""

    def __init__(
        self,
        runtime_dir: str | Path,
        model_file: str | Path,
        *,
        num_threads: int = 4,
    ) -> None:
        root = Path(runtime_dir).resolve()
        executable = root / "whisper-cli.exe"
        model = Path(model_file).resolve()
        if not executable.is_file():
            raise FileNotFoundError(f"whisper-cli.exe not found: {executable}")
        if not model.is_file():
            raise FileNotFoundError(f"whisper model not found: {model}")
        self.executable = executable
        self.model = model
        self.num_threads = num_threads
        self.model_version = f"whisper.cpp-{root.parent.name}-{model.stem}"

    def transcribe(self, wav_path: str | Path) -> Transcript:
        completed = subprocess.run(
            [
                str(self.executable),
                "-m", str(self.model),
                "-f", str(Path(wav_path).resolve()),
                "-l", "ko",
                "-t", str(self.num_threads),
                "-np",
                "-nt",
                "-ng",
            ],
            cwd=self.executable.parent,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        text = completed.stdout.strip()
        if text.startswith("-"):
            text = text[1:].lstrip()
        return Transcript(text, self.model_version)


class WhisperCppHttpBackend:
    """Persistent whisper.cpp server client; model load is outside each timing."""

    def __init__(self, server_url: str, *, model_version: str) -> None:
        self.endpoint = server_url.rstrip("/") + "/inference"
        self.model_version = model_version

    def transcribe(self, wav_path: str | Path) -> Transcript:
        try:
            import requests
        except ImportError as exc:
            raise RuntimeError("install the base 'requests' dependency") from exc
        source = Path(wav_path)
        with source.open("rb") as handle:
            response = requests.post(
                self.endpoint,
                files={"file": (source.name, handle, "audio/wav")},
                data={"response_format": "json", "language": "ko"},
                timeout=60,
            )
        response.raise_for_status()
        text = str(response.json().get("text", "")).strip()
        if text.startswith("-"):
            text = text[1:].lstrip()
        return Transcript(text, self.model_version)


def create_backend(
    name: str,
    model_dir: str | Path,
    *,
    threads: int = 4,
    model_file: str | Path | None = None,
    server_url: str | None = None,
    model_version: str | None = None,
):
    if name == "vosk":
        if model_dir is None:
            raise ValueError("vosk backend requires --model-dir")
        return VoskBackend(model_dir)
    if name == "sherpa":
        if model_dir is None:
            raise ValueError("sherpa backend requires --model-dir")
        return SherpaOfflineBackend(model_dir, num_threads=threads)
    if name == "whisper":
        if model_dir is None or model_file is None:
            raise ValueError("whisper backend requires --model-dir and --model-file")
        return WhisperCppBackend(model_dir, model_file, num_threads=threads)
    if name == "whisper-server":
        if not server_url or not model_version:
            raise ValueError(
                "whisper-server backend requires --server-url and --model-version"
            )
        return WhisperCppHttpBackend(server_url, model_version=model_version)
    raise ValueError(f"unsupported backend: {name}")
