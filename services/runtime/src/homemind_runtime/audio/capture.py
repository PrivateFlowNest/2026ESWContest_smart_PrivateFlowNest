"""One audio capture (microphone or WAV file) fanned out to several in-process consumers.

Blocks are 16 kHz mono float32, 100 ms each. Every consumer gets its own queue so the context
path (YAMNet every second) and the voice path (VAD + Whisper) never compete for the device.
Microphone mode drops the oldest block of a lagging consumer; file mode never drops.
"""

from __future__ import annotations

import logging
import queue
import threading
import time
from dataclasses import dataclass
from math import gcd
from pathlib import Path
from typing import Any

import numpy as np

from .vad import SAMPLE_RATE

log = logging.getLogger(__name__)
BLOCK_SECONDS = 0.1


@dataclass(frozen=True)
class Block:
    samples: np.ndarray
    start_sample: int  # index of samples[0] since capture start


class Subscriber:
    def __init__(self, name: str, *, maxsize: int = 0) -> None:
        self.name = name
        self.queue: queue.Queue[Block | None] = queue.Queue(maxsize=maxsize)
        self.dropped = 0

    def get(self, timeout: float | None = None) -> Block | None:
        return self.queue.get(timeout=timeout)


class AudioFanout:
    def __init__(self, *, lossy: bool) -> None:
        self.lossy = lossy
        self._subscribers: list[Subscriber] = []
        self.position = 0
        self.started_at = time.time()

    def subscribe(self, name: str) -> Subscriber:
        # 300 blocks = 30 s of backlog before a live consumer starts losing audio.
        subscriber = Subscriber(name, maxsize=300 if self.lossy else 0)
        self._subscribers.append(subscriber)
        return subscriber

    def push(self, samples: np.ndarray) -> None:
        block = Block(np.asarray(samples, dtype=np.float32), self.position)
        self.position += block.samples.size
        for subscriber in self._subscribers:
            if not self.lossy:
                subscriber.queue.put(block)
                continue
            try:
                subscriber.queue.put_nowait(block)
            except queue.Full:
                try:
                    subscriber.queue.get_nowait()
                    subscriber.dropped += 1
                except queue.Empty:
                    pass
                subscriber.queue.put_nowait(block)

    def close(self) -> None:
        for subscriber in self._subscribers:
            if self.lossy:
                try:
                    subscriber.queue.put_nowait(None)
                except queue.Full:
                    subscriber.queue.get_nowait()
                    subscriber.queue.put_nowait(None)
            else:
                subscriber.queue.put(None)


def list_input_devices() -> list[dict[str, Any]]:
    import sounddevice as sd

    apis = sd.query_hostapis()
    rows = []
    for index, device in enumerate(sd.query_devices()):
        if int(device["max_input_channels"]) <= 0:
            continue
        rows.append({
            "index": index,
            "name": str(device["name"]),
            "hostapi": str(apis[int(device["hostapi"])]["name"]),
            "default_samplerate": float(device["default_samplerate"]),
            "default": index == sd.default.device[0],
        })
    return rows


def resolve_device(spec: int | str | None) -> int | None:
    """None/'' -> default input; int or digit string -> index; other string -> first name match."""

    if spec is None or spec == "":
        return None
    if isinstance(spec, int) or str(spec).strip().isdigit():
        return int(spec)
    needle = str(spec).casefold()
    for row in list_input_devices():
        if needle in row["name"].casefold():
            return int(row["index"])
    raise ValueError(f"no input device matches {spec!r}; run with --list-devices")


class MicrophoneSource:
    def __init__(self, fanout: AudioFanout, device: int | str | None) -> None:
        self.fanout = fanout
        self.device = resolve_device(device)
        self._raw: queue.Queue[np.ndarray] = queue.Queue(maxsize=200)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._stream = None
        self.native_rate = SAMPLE_RATE

    def start(self) -> None:
        import sounddevice as sd

        try:
            sd.check_input_settings(device=self.device, channels=1, dtype="float32", samplerate=SAMPLE_RATE)
            self.native_rate = SAMPLE_RATE
        except Exception:  # noqa: BLE001 - PortAudio raises several types for unsupported rates
            self.native_rate = int(round(sd.query_devices(self.device, "input")["default_samplerate"]))
        blocksize = int(round(self.native_rate * BLOCK_SECONDS))
        name = sd.query_devices(self.device, "input")["name"]
        log.info("microphone device=%s (%s) native_rate=%d", self.device, name, self.native_rate)

        def callback(indata, frames, time_info, status) -> None:
            del frames, time_info
            if status:
                log.debug("audio status %s", status)
            chunk = np.asarray(indata[:, 0], dtype=np.float32).copy()
            try:
                self._raw.put_nowait(chunk)
            except queue.Full:
                pass

        self._stream = sd.InputStream(samplerate=self.native_rate, channels=1, dtype="float32",
                                      device=self.device, blocksize=blocksize, callback=callback)
        self._thread = threading.Thread(target=self._pump, name="mic-pump", daemon=True)
        self.fanout.started_at = time.time()
        self._stream.start()
        self._thread.start()

    def _pump(self) -> None:
        from scipy.signal import resample_poly

        divisor = gcd(self.native_rate, SAMPLE_RATE)
        up, down = SAMPLE_RATE // divisor, self.native_rate // divisor
        while not self._stop.is_set():
            try:
                chunk = self._raw.get(timeout=0.2)
            except queue.Empty:
                continue
            if self.native_rate != SAMPLE_RATE:
                chunk = resample_poly(chunk, up, down).astype(np.float32)
            self.fanout.push(np.clip(chunk, -1.0, 1.0))
        self.fanout.close()

    def stop(self) -> None:
        self._stop.set()
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def join(self, timeout: float | None = None) -> None:
        if self._thread is not None:
            self._thread.join(timeout)


class WavFileSource:
    """Feed WAV/FLAC files (in order) through the fanout, real-time or as fast as possible."""

    def __init__(self, fanout: AudioFanout, paths: list[str | Path], *, realtime: bool = True,
                 offset_seconds: float = 0.0, max_seconds: float | None = None) -> None:
        self.fanout = fanout
        self.paths = [Path(path) for path in paths]
        self.realtime = realtime
        self.offset_seconds = float(offset_seconds or 0.0)
        self.max_seconds = float(max_seconds) if max_seconds else None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.total_samples = 0
        self.waveform = self._load()

    def _load(self) -> np.ndarray:
        from homemind_audio.audio import load_wav_16k_mono, load_wav_segment_16k_mono

        def load(path: Path) -> np.ndarray:
            if self.offset_seconds <= 0 and self.max_seconds is None:
                return load_wav_16k_mono(path)
            # Segment read keeps hour-long recordings (AIHub meetings) out of memory.
            end = self.offset_seconds + (self.max_seconds or 24 * 3600.0)
            return load_wav_segment_16k_mono(path, self.offset_seconds, end)

        parts = [load(path) for path in self.paths]
        waveform = np.concatenate(parts) if parts else np.zeros(0, dtype=np.float32)
        self.total_samples = int(waveform.size)
        return waveform

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="wav-source", daemon=True)
        self.fanout.started_at = time.time()
        self._thread.start()

    def _run(self) -> None:
        block = int(SAMPLE_RATE * BLOCK_SECONDS)
        began = time.perf_counter()
        for offset in range(0, self.waveform.size, block):
            if self._stop.is_set():
                break
            self.fanout.push(self.waveform[offset : offset + block])
            if self.realtime:
                due = began + (offset + block) / SAMPLE_RATE
                delay = due - time.perf_counter()
                if delay > 0:
                    self._stop.wait(delay)
        self.fanout.close()

    def stop(self) -> None:
        self._stop.set()

    def join(self, timeout: float | None = None) -> None:
        if self._thread is not None:
            self._thread.join(timeout)
