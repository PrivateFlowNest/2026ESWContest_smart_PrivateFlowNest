"""Hybrid real-time inference and deterministic context stabilization.

The module deliberately keeps device policy out of the model path.  It emits the
existing Pi 5 -> Pi 4 audio-context contract; Pi 4 remains responsible for
deciding which appliances may actually change state.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from math import gcd
from pathlib import Path
from queue import Empty, Full, Queue
from typing import Iterable, Iterator

import joblib
import numpy as np
from scipy.signal import resample_poly

from .audio import TARGET_SAMPLE_RATE, vad_temporal_features
from .labels import QUIET_REQUIRED_CONTEXTS
from .models import Prediction, prediction_to_event
from .yamnet import (
    FEATURE_VERSION_MEAN_VAD_V1,
    FEATURE_VERSION_TEMPORAL_V2,
    YAMNET_HANDLE,
    YamnetEmbeddingExtractor,
)

_MEAN_DIMENSIONS = 1024
_VAD_DIMENSIONS = 12


@dataclass(frozen=True)
class FusionConfig:
    """Initial safe defaults; tune these with the actual Pi 5 microphone."""

    activity_confidence: float = 0.65
    enter_quiet_count: int = 3
    exit_quiet_count: int = 2
    other_transition_count: int = 3
    heartbeat_seconds: float = 5.0
    context_ttl_seconds: int = 10
    sound_confidence: float = 0.80
    sound_confirmation_count: int = 2
    sound_cooldown_seconds: float = 8.0

    def __post_init__(self) -> None:
        for name in ("activity_confidence", "sound_confidence"):
            value = float(getattr(self, name))
            if not 0.0 <= value <= 1.0:
                raise ValueError(f"{name} must be between 0 and 1")
        for name in (
            "enter_quiet_count",
            "exit_quiet_count",
            "other_transition_count",
            "context_ttl_seconds",
            "sound_confirmation_count",
        ):
            if int(getattr(self, name)) < 1:
                raise ValueError(f"{name} must be positive")
        if self.heartbeat_seconds <= 0 or self.sound_cooldown_seconds < 0:
            raise ValueError("heartbeat must be positive and cooldown non-negative")


@dataclass(frozen=True)
class InferenceFrame:
    activity: Prediction
    sound_event: Prediction
    events: tuple[dict[str, object], ...]
    timestamp: datetime
    speech_ratio: float


class _LoadedHead:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        bundle = joblib.load(self.path)
        self.pipeline = bundle["pipeline"]
        self.head = str(bundle["head"])
        self.model_version = str(bundle["model_version"])
        self.yamnet_handle = str(bundle.get("yamnet_handle", YAMNET_HANDLE))
        self.feature_version = str(
            bundle.get("feature_version", FEATURE_VERSION_MEAN_VAD_V1)
        )

    def predict(self, vector: np.ndarray) -> Prediction:
        row = np.asarray(vector, dtype=np.float32).reshape(1, -1)
        expected = getattr(self.pipeline, "n_features_in_", row.shape[1])
        if row.shape[1] != expected:
            raise ValueError(
                f"{self.path} expects {expected} features, got {row.shape[1]}"
            )
        probabilities = self.pipeline.predict_proba(row)[0]
        best_index = int(np.argmax(probabilities))
        return Prediction(
            head=self.head,
            label=str(self.pipeline.classes_[best_index]),
            confidence=float(probabilities[best_index]),
            model_version=self.model_version,
            class_probabilities={
                str(label): float(probability)
                for label, probability in zip(self.pipeline.classes_, probabilities)
            },
        )


def adapt_feature_vector(
    vector: np.ndarray,
    *,
    source_version: str,
    target_version: str,
) -> np.ndarray:
    """Reuse a temporal v4 extraction for a mean+VAD v3 classifier."""

    values = np.asarray(vector, dtype=np.float32).reshape(-1)
    if source_version == target_version:
        return values
    if (
        source_version == FEATURE_VERSION_TEMPORAL_V2
        and target_version == FEATURE_VERSION_MEAN_VAD_V1
    ):
        expected = (_MEAN_DIMENSIONS * 3) + _VAD_DIMENSIONS
        if values.size != expected:
            raise ValueError(f"Expected {expected} temporal features, got {values.size}")
        return np.concatenate(
            [values[:_MEAN_DIMENSIONS], values[-_VAD_DIMENSIONS:]]
        ).astype(np.float32, copy=False)
    raise ValueError(
        f"Cannot adapt features from {source_version!r} to {target_version!r}"
    )


class HybridAudioPredictor:
    """Load v3 activity and v4 sound-event heads with one shared YAMNet pass."""

    def __init__(
        self,
        activity_model: str | Path,
        sound_model: str | Path,
        *,
        extractor: object | None = None,
    ) -> None:
        self.activity = _LoadedHead(activity_model)
        self.sound_event = _LoadedHead(sound_model)
        if self.activity.head != "activity":
            raise ValueError(f"Expected activity model, got {self.activity.head!r}")
        if self.sound_event.head != "sound_event":
            raise ValueError(
                f"Expected sound_event model, got {self.sound_event.head!r}"
            )
        if self.activity.yamnet_handle != self.sound_event.yamnet_handle:
            raise ValueError("The two heads use different YAMNet model handles")

        versions = {self.activity.feature_version, self.sound_event.feature_version}
        self.feature_version = (
            FEATURE_VERSION_TEMPORAL_V2
            if FEATURE_VERSION_TEMPORAL_V2 in versions
            else FEATURE_VERSION_MEAN_VAD_V1
        )
        self.extractor = extractor or YamnetEmbeddingExtractor(
            self.activity.yamnet_handle,
            feature_version=self.feature_version,
        )
        actual_version = str(getattr(self.extractor, "feature_version", ""))
        if actual_version != self.feature_version:
            raise ValueError(
                f"Extractor uses {actual_version!r}, expected {self.feature_version!r}"
            )

    def predict_waveform(self, waveform: np.ndarray) -> tuple[Prediction, Prediction]:
        shared = self.extractor.extract_waveform(waveform)
        activity_features = adapt_feature_vector(
            shared,
            source_version=self.feature_version,
            target_version=self.activity.feature_version,
        )
        sound_features = adapt_feature_vector(
            shared,
            source_version=self.feature_version,
            target_version=self.sound_event.feature_version,
        )
        return (
            self.activity.predict(activity_features),
            self.sound_event.predict(sound_features),
        )


class ContextStabilizer:
    """Turn raw overlapping-window predictions into contract events."""

    def __init__(self, config: FusionConfig | None = None) -> None:
        self.config = config or FusionConfig()
        self._activity = Prediction("activity", "UNKNOWN", 0.0, "uninitialized")
        self._candidate = "UNKNOWN"
        self._candidate_predictions: deque[Prediction] = deque(maxlen=max(
            self.config.enter_quiet_count,
            self.config.exit_quiet_count,
            self.config.other_transition_count,
        ))
        self._last_activity_emitted_at: datetime | None = None
        self._sound_candidate: str | None = None
        self._sound_streak = 0
        self._sound_last_emitted_at: dict[str, datetime] = {}

    @property
    def activity(self) -> Prediction:
        return self._activity

    def _required_activity_count(self, candidate: str) -> int:
        current_quiet = self._activity.label in QUIET_REQUIRED_CONTEXTS
        candidate_quiet = candidate in QUIET_REQUIRED_CONTEXTS
        if current_quiet and not candidate_quiet:
            return self.config.exit_quiet_count
        if not current_quiet and candidate_quiet:
            return self.config.enter_quiet_count
        return self.config.other_transition_count

    def _activity_event(
        self,
        *,
        trace_id: str,
        now: datetime,
        consensus_count: int,
    ) -> dict[str, object]:
        return prediction_to_event(
            self._activity,
            trace_id=trace_id,
            evidence=[
                "yamnet_embedding",
                f"temporal_consensus:{consensus_count}",
                f"activity_threshold:{self.config.activity_confidence:.2f}",
            ],
            now=now,
            context_ttl_seconds=self.config.context_ttl_seconds,
        )

    def _process_activity(
        self,
        prediction: Prediction,
        *,
        trace_id: str,
        now: datetime,
    ) -> dict[str, object] | None:
        candidate = (
            prediction.label
            if prediction.confidence >= self.config.activity_confidence
            else "UNKNOWN"
        )
        if candidate != self._candidate:
            self._candidate = candidate
            self._candidate_predictions.clear()
        self._candidate_predictions.append(prediction)

        required = self._required_activity_count(candidate)
        changed = candidate != self._activity.label and len(
            self._candidate_predictions
        ) >= required
        if changed:
            confidence = (
                float(np.mean([item.confidence for item in self._candidate_predictions]))
                if candidate != "UNKNOWN"
                else 0.0
            )
            self._activity = Prediction(
                "activity", candidate, confidence, prediction.model_version
            )
            self._last_activity_emitted_at = now
            return self._activity_event(
                trace_id=trace_id, now=now, consensus_count=required
            )

        heartbeat_due = (
            self._activity.label != "UNKNOWN"
            and self._last_activity_emitted_at is not None
            and now - self._last_activity_emitted_at
            >= timedelta(seconds=self.config.heartbeat_seconds)
        )
        if heartbeat_due:
            self._last_activity_emitted_at = now
            return self._activity_event(
                trace_id=trace_id,
                now=now,
                consensus_count=min(len(self._candidate_predictions), required),
            )
        return None

    def _process_sound(
        self,
        prediction: Prediction,
        *,
        trace_id: str,
        now: datetime,
    ) -> dict[str, object] | None:
        candidate = (
            prediction.label
            if prediction.label != "OTHER"
            and prediction.confidence >= self.config.sound_confidence
            else None
        )
        if candidate is None:
            self._sound_candidate = None
            self._sound_streak = 0
            return None
        if candidate == self._sound_candidate:
            self._sound_streak += 1
        else:
            self._sound_candidate = candidate
            self._sound_streak = 1
        if self._sound_streak < self.config.sound_confirmation_count:
            return None

        previous = self._sound_last_emitted_at.get(candidate)
        if previous is not None and now - previous < timedelta(
            seconds=self.config.sound_cooldown_seconds
        ):
            return None
        self._sound_last_emitted_at[candidate] = now
        return prediction_to_event(
            prediction,
            trace_id=trace_id,
            evidence=[
                "yamnet_embedding",
                f"temporal_consensus:{self.config.sound_confirmation_count}",
                f"sound_threshold:{self.config.sound_confidence:.2f}",
            ],
            now=now,
        )

    def process(
        self,
        activity: Prediction,
        sound_event: Prediction,
        *,
        trace_id: str,
        now: datetime | None = None,
    ) -> tuple[dict[str, object], ...]:
        timestamp = now or datetime.now(timezone.utc)
        emitted = [
            self._process_activity(activity, trace_id=trace_id, now=timestamp),
            self._process_sound(sound_event, trace_id=trace_id, now=timestamp),
        ]
        return tuple(event for event in emitted if event is not None)


class LiveAudioContextEngine:
    def __init__(
        self,
        predictor: HybridAudioPredictor,
        *,
        config: FusionConfig | None = None,
    ) -> None:
        self.predictor = predictor
        self.stabilizer = ContextStabilizer(config)

    def process_waveform(
        self,
        waveform: np.ndarray,
        *,
        trace_id: str,
        now: datetime | None = None,
    ) -> InferenceFrame:
        timestamp = now or datetime.now(timezone.utc)
        activity, sound_event = self.predictor.predict_waveform(waveform)
        events = self.stabilizer.process(
            activity, sound_event, trace_id=trace_id, now=timestamp
        )
        speech_ratio = float(vad_temporal_features(waveform)[0])
        return InferenceFrame(
            activity,
            sound_event,
            events,
            timestamp,
            speech_ratio,
        )


def iter_waveform_windows(
    waveform: np.ndarray,
    *,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
) -> Iterator[np.ndarray]:
    if window_seconds <= 0 or hop_seconds <= 0 or hop_seconds > window_seconds:
        raise ValueError("Require 0 < hop_seconds <= window_seconds")
    window = int(round(window_seconds * TARGET_SAMPLE_RATE))
    hop = int(round(hop_seconds * TARGET_SAMPLE_RATE))
    values = np.asarray(waveform, dtype=np.float32).reshape(-1)
    if values.size < window:
        values = np.pad(values, (0, window - values.size))
    for start in range(0, values.size - window + 1, hop):
        yield values[start : start + window]


def iter_microphone_windows(
    *,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
    device: int | str | None = None,
) -> Iterator[np.ndarray]:
    """Yield gap-free rolling windows while inference runs in the caller."""

    if window_seconds <= 0 or hop_seconds <= 0 or hop_seconds > window_seconds:
        raise ValueError("Require 0 < hop_seconds <= window_seconds")
    try:
        import sounddevice as sound_device
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise RuntimeError("sounddevice is not installed") from exc

    native_rate = TARGET_SAMPLE_RATE
    try:
        sound_device.check_input_settings(
            device=device, channels=1, dtype="float32", samplerate=native_rate
        )
    except sound_device.PortAudioError:
        native_rate = int(
            round(sound_device.query_devices(device, "input")["default_samplerate"])
        )

    window = int(round(window_seconds * native_rate))
    hop = int(round(hop_seconds * native_rate))
    chunks: Queue[np.ndarray] = Queue(
        maxsize=max(8, int(round(window_seconds / hop_seconds)) * 4)
    )

    def callback(indata, frames, time_info, status) -> None:
        del frames, time_info, status
        chunk = np.asarray(indata[:, 0], dtype=np.float32).copy()
        try:
            chunks.put_nowait(chunk)
        except Full:
            try:
                chunks.get_nowait()
            except Empty:
                pass
            chunks.put_nowait(chunk)

    buffer = np.empty(0, dtype=np.float32)
    blocksize = max(256, min(hop, int(round(native_rate * 0.1))))
    with sound_device.InputStream(
        samplerate=native_rate,
        channels=1,
        dtype="float32",
        device=device,
        blocksize=blocksize,
        callback=callback,
    ):
        while True:
            buffer = np.concatenate([buffer, chunks.get()])
            while buffer.size >= window:
                current = buffer[:window]
                buffer = buffer[hop:]
                if native_rate != TARGET_SAMPLE_RATE:
                    divisor = gcd(native_rate, TARGET_SAMPLE_RATE)
                    current = resample_poly(
                        current,
                        TARGET_SAMPLE_RATE // divisor,
                        native_rate // divisor,
                    ).astype(np.float32)
                yield np.clip(current, -1.0, 1.0).astype(
                    np.float32, copy=False
                )


def run_windows(
    engine: LiveAudioContextEngine,
    windows: Iterable[np.ndarray],
    *,
    trace_id: str,
    max_windows: int | None = None,
    base_time: datetime | None = None,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
) -> Iterator[InferenceFrame]:
    for index, waveform in enumerate(windows, start=1):
        if max_windows is not None and index > max_windows:
            break
        timestamp = (
            base_time
            + timedelta(seconds=window_seconds + (index - 1) * hop_seconds)
            if base_time is not None
            else None
        )
        yield engine.process_waveform(waveform, trace_id=trace_id, now=timestamp)
