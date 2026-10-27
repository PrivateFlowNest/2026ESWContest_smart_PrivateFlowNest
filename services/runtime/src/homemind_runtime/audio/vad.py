"""Energy VAD endpointing for the voice path (16 kHz mono float32 blocks).

Deliberately simple and deterministic: frame RMS in dBFS against an adaptive noise floor.
An utterance starts once ~start_ms of the last start_ms is voiced, ends after end_silence_ms of
silence or at max_ms, and is dropped when shorter than min_ms of speech. ``armed=False`` keeps the
noise floor tracking but never starts an utterance by itself (push-to-talk idle state); ``begin()``
then opens an utterance at the key press (with pre-roll) and silence only ends it after a speech
onset was seen, so a slow speaker is not cut off before they start.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Any, Mapping

import numpy as np

SAMPLE_RATE = 16_000


@dataclass(frozen=True)
class EndpointConfig:
    frame_ms: int = 20
    start_ms: int = 200
    start_ratio: float = 0.8
    end_silence_ms: int = 700
    max_ms: int = 8000
    min_ms: int = 400
    pre_roll_ms: int = 300
    tail_ms: int = 200
    threshold_db: float = -45.0
    noise_margin_db: float = 10.0
    noise_adapt: float = 0.05

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any] | None) -> "EndpointConfig":
        value = dict(value or {})
        unknown = set(value) - set(cls.__dataclass_fields__)
        if unknown:
            raise ValueError(f"unknown vad fields: {sorted(unknown)}")
        return cls(**value)

    def __post_init__(self) -> None:
        if self.frame_ms <= 0 or self.start_ms < self.frame_ms or self.end_silence_ms < self.frame_ms:
            raise ValueError("vad: frame_ms > 0 and start/end windows of at least one frame required")
        if not self.min_ms <= self.max_ms:
            raise ValueError("vad: min_ms must be <= max_ms")
        if not 0.0 < self.start_ratio <= 1.0 or not 0.0 < self.noise_adapt <= 1.0:
            raise ValueError("vad: start_ratio and noise_adapt must be in (0, 1]")


@dataclass(frozen=True)
class Utterance:
    samples: np.ndarray
    start_sample: int
    end_sample: int
    reason: str  # silence | max_length | manual | flush

    @property
    def seconds(self) -> float:
        return self.samples.size / SAMPLE_RATE


class EnergyEndpointer:
    def __init__(self, config: EndpointConfig | None = None, *, armed: bool = True) -> None:
        self.config = config or EndpointConfig()
        cfg = self.config
        self.frame = SAMPLE_RATE * cfg.frame_ms // 1000
        self._start_frames = max(1, cfg.start_ms // cfg.frame_ms)
        self._end_frames = max(1, cfg.end_silence_ms // cfg.frame_ms)
        self._max_frames = max(1, cfg.max_ms // cfg.frame_ms)
        self._min_frames = max(1, cfg.min_ms // cfg.frame_ms)
        self._tail_frames = cfg.tail_ms // cfg.frame_ms
        self._pre_frames = max(self._start_frames, cfg.pre_roll_ms // cfg.frame_ms)
        self.armed = armed
        self.noise_floor_db: float | None = None
        self._pending = np.empty(0, dtype=np.float32)
        self._position = 0  # samples consumed into frames
        self._history: deque[tuple[np.ndarray, bool]] = deque(maxlen=self._pre_frames)
        self._recent: deque[bool] = deque(maxlen=self._start_frames)
        self._active: list[np.ndarray] | None = None
        self._active_start = 0
        self._silence_run = 0
        self._voiced_frames = 0
        self._onset_seen = False

    @property
    def in_speech(self) -> bool:
        return self._active is not None

    @property
    def onset_seen(self) -> bool:
        return self._active is not None and self._onset_seen

    def begin(self) -> None:
        """Open an utterance now (push-to-talk), keeping the pre-roll history."""

        if self._active is not None:
            return
        frames = [item for item, _ in self._history]
        self._active = frames
        self._active_start = self._position - sum(item.size for item in frames)
        self._voiced_frames = sum(1 for _, flag in self._history if flag)
        self._silence_run = 0
        self._onset_seen = False
        self._history.clear()
        self._recent.clear()

    def threshold_db(self) -> float:
        floor = self.noise_floor_db if self.noise_floor_db is not None else -90.0
        return max(self.config.threshold_db, floor + self.config.noise_margin_db)

    def feed(self, samples: np.ndarray) -> list[Utterance]:
        values = np.asarray(samples, dtype=np.float32).reshape(-1)
        self._pending = np.concatenate([self._pending, values]) if self._pending.size else values.copy()
        finished: list[Utterance] = []
        offset = 0
        while self._pending.size - offset >= self.frame:
            frame = self._pending[offset : offset + self.frame]
            offset += self.frame
            result = self._frame(frame)
            if result is not None:
                finished.append(result)
        self._pending = self._pending[offset:].copy()
        return finished

    def stop(self, reason: str = "manual") -> Utterance | None:
        """End the current utterance now (push-to-talk second Enter, end of file)."""

        if self._active is None:
            return None
        return self._finish(reason, trim_silence=False)

    def reset(self) -> None:
        self._active = None
        self._recent.clear()
        self._silence_run = 0
        self._voiced_frames = 0
        self._onset_seen = False

    def _frame(self, frame: np.ndarray) -> Utterance | None:
        rms = float(np.sqrt(np.mean(np.square(frame, dtype=np.float64)) + 1e-12))
        level = 20.0 * np.log10(rms + 1e-10)
        voiced = level >= self.threshold_db()
        start = self._position
        self._position += frame.size
        if self._active is None:
            if not voiced or self.noise_floor_db is None:
                alpha = 1.0 if self.noise_floor_db is None else self.config.noise_adapt
                current = self.noise_floor_db if self.noise_floor_db is not None else level
                self.noise_floor_db = max(-90.0, (1.0 - alpha) * current + alpha * level)
            self._history.append((frame.copy(), voiced))
            if not self.armed:
                self._recent.clear()
                return None
            self._recent.append(voiced)
            needed = int(np.ceil(self.config.start_ratio * self._start_frames))
            if len(self._recent) == self._start_frames and sum(self._recent) >= needed:
                frames = [item for item, _ in self._history]
                self._active = frames
                self._active_start = start + frame.size - sum(item.size for item in frames)
                self._voiced_frames = sum(1 for _, flag in self._history if flag)
                self._silence_run = 0
                self._onset_seen = True
                self._history.clear()
                self._recent.clear()
            return None
        self._active.append(frame.copy())
        if voiced:
            self._voiced_frames += 1
            self._silence_run = 0
        else:
            self._silence_run += 1
        if not self._onset_seen:
            self._recent.append(voiced)
            needed = int(np.ceil(self.config.start_ratio * self._start_frames))
            if len(self._recent) == self._start_frames and sum(self._recent) >= needed:
                self._onset_seen = True
        if self._onset_seen and self._silence_run >= self._end_frames:
            return self._finish("silence", trim_silence=True)
        if len(self._active) >= self._max_frames:
            return self._finish("max_length", trim_silence=False)
        return None

    def _finish(self, reason: str, *, trim_silence: bool) -> Utterance | None:
        frames = self._active or []
        if trim_silence:
            keep = len(frames) - self._silence_run + self._tail_frames
            frames = frames[: max(0, min(len(frames), keep))]
        voiced = self._voiced_frames
        start = self._active_start
        self._active = None
        self._silence_run = 0
        self._voiced_frames = 0
        self._onset_seen = False
        self._recent.clear()
        if voiced < self._min_frames or not frames:
            return None
        samples = np.concatenate(frames).astype(np.float32, copy=False)
        return Utterance(samples, start, start + samples.size, reason)
