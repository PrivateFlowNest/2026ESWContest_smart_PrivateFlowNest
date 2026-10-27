"""End-to-end WAV/YAMNet to fused context adapter.

Heavy audio dependencies are imported only when the real model runner is built, so
the context rules and contracts remain independently testable.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from collections import Counter
import statistics
import sys
import time
from typing import Any, Callable, Iterable, Iterator

from .engine import ContextFusionEngine
from .models import ContextObservation
from .validation import validate_event


SERVICE_ROOT = Path(__file__).resolve().parents[2]
EDGE_AI_ROOT = SERVICE_ROOT.parent
AUDIO_CONTEXT_ROOT = EDGE_AI_ROOT / "audio-context"
DEFAULT_ACTIVITY_MODEL = (
    AUDIO_CONTEXT_ROOT
    / "artifacts"
    / "public-balanced-v6-ensemble-50-50"
    / "activity.joblib"
)
DEFAULT_SOUND_MODEL = (
    AUDIO_CONTEXT_ROOT
    / "artifacts"
    / "public-balanced-v5-window4s"
    / "sound_event.joblib"
)


@dataclass(frozen=True)
class FusionInputDefaults:
    presence_zone: str = "UNKNOWN"
    presence_confidence: float = 0.0
    turns_per_minute: float | None = None
    turn_taking_method: str = "UNKNOWN"
    tv_on: bool | None = None
    laptop_active: bool | None = None
    headphones_connected: bool | None = None


@dataclass(frozen=True)
class AudioFusionFrame:
    window_index: int
    offset_seconds: float
    activity_prediction: dict[str, object]
    sound_prediction: dict[str, object]
    context_event: dict[str, object]
    latency_ms: float

    def debug_record(self) -> dict[str, object]:
        return {
            "window_index": self.window_index,
            "offset_seconds": self.offset_seconds,
            "activity_prediction": self.activity_prediction,
            "sound_prediction": self.sound_prediction,
            "context_event": self.context_event,
            "latency_ms": self.latency_ms,
        }


class AudioFusionRunner:
    """Feed raw audio-model predictions to the independent fusion state machine."""

    def __init__(
        self,
        predictor: Any,
        speech_ratio: Callable[[Any], float],
        *,
        engine: ContextFusionEngine | None = None,
    ) -> None:
        self.predictor = predictor
        self.speech_ratio = speech_ratio
        self.engine = engine or ContextFusionEngine()

    def process_windows(
        self,
        windows: Iterable[Any],
        *,
        trace_id: str,
        base_time: datetime,
        hop_seconds: float,
        window_seconds: float,
        defaults: FusionInputDefaults | None = None,
        max_windows: int | None = None,
    ) -> Iterator[AudioFusionFrame]:
        if hop_seconds <= 0 or window_seconds <= 0:
            raise ValueError("window_seconds and hop_seconds must be positive")
        settings = defaults or FusionInputDefaults()
        started_at = base_time
        if started_at.tzinfo is None:
            started_at = started_at.replace(tzinfo=timezone.utc)
        started_at = started_at.astimezone(timezone.utc)

        for index, window in enumerate(windows):
            if max_windows is not None and index >= max_windows:
                break
            started = time.perf_counter_ns()
            activity, sound = self.predictor.predict_waveform(window)
            # A window beginning at offset N is actionable only after the complete
            # window has arrived, so event time is the window end, not its start.
            timestamp = started_at + timedelta(
                seconds=window_seconds + index * hop_seconds
            )
            observation = ContextObservation(
                audio_context=activity.label,
                audio_confidence=float(activity.confidence),
                audio_class_probabilities=getattr(
                    activity, "class_probabilities", None
                ),
                audio_model_version=str(activity.model_version),
                speech_ratio=float(self.speech_ratio(window)),
                turns_per_minute=settings.turns_per_minute,
                turn_taking_method=settings.turn_taking_method,
                duration_seconds=window_seconds + index * hop_seconds,
                presence_zone=settings.presence_zone,
                presence_confidence=settings.presence_confidence,
                tv_on=settings.tv_on,
                laptop_active=settings.laptop_active,
                headphones_connected=settings.headphones_connected,
                trace_id=trace_id,
                timestamp=timestamp,
            )
            event = self.engine.update(observation)
            validate_event(event)
            latency_ms = (time.perf_counter_ns() - started) / 1_000_000
            yield AudioFusionFrame(
                window_index=index + 1,
                offset_seconds=round(index * hop_seconds, 3),
                activity_prediction={
                    "label": activity.label,
                    "confidence": round(float(activity.confidence), 6),
                    "model_version": str(activity.model_version),
                },
                sound_prediction={
                    "label": sound.label,
                    "confidence": round(float(sound.confidence), 6),
                    "model_version": str(sound.model_version),
                },
                context_event=event,
                latency_ms=round(latency_ms, 3),
            )


def summarize_frames(frames: Iterable[AudioFusionFrame], *, window_seconds: float) -> dict[str, object]:
    values = list(frames)
    if not values:
        raise ValueError("at least one frame is required")
    timings = sorted(item.latency_ms for item in values)
    warm = sorted(item.latency_ms for item in values[1:]) or timings

    def percentile(samples: list[float], fraction: float) -> float:
        return samples[min(len(samples) - 1, int(len(samples) * fraction))]

    transitions = [
        {
            "window": item.window_index,
            "decision_seconds": round(item.offset_seconds + window_seconds, 3),
            "transition": item.context_event["transition"],
            "context_type": item.context_event["context_type"],
            "quiet_required": item.context_event["quiet_required"],
        }
        for item in values
        if item.context_event["transition"] != "UNCHANGED"
    ]
    return {
        "schema_version": "1.0",
        "windows": len(values),
        "window_seconds": window_seconds,
        "activity_prediction_counts": dict(
            Counter(str(item.activity_prediction["label"]) for item in values)
        ),
        "fused_context_counts": dict(
            Counter(str(item.context_event["context_type"]) for item in values)
        ),
        "quiet_required_ever": any(
            bool(item.context_event["quiet_required"]) for item in values
        ),
        "transitions": transitions,
        "latency_ms": {
            "cold_first": round(values[0].latency_ms, 3),
            "all_mean": round(statistics.fmean(timings), 3),
            "all_p95": round(percentile(timings, 0.95), 3),
            "warm_mean": round(statistics.fmean(warm), 3),
            "warm_p95": round(percentile(warm, 0.95), 3),
        },
        "privacy": {
            "raw_audio_persisted": False,
            "transcript_persisted": False,
            "image_persisted": False,
        },
    }


def _import_audio_context():
    try:
        from homemind_audio.audio import (  # type: ignore
            load_wav_16k_mono,
            load_wav_segment_16k_mono,
            vad_temporal_features,
        )
        from homemind_audio.realtime import (  # type: ignore
            HybridAudioPredictor,
            iter_waveform_windows,
        )
    except ImportError:
        source = AUDIO_CONTEXT_ROOT / "src"
        if str(source) not in sys.path:
            sys.path.insert(0, str(source))
        from homemind_audio.audio import (  # type: ignore
            load_wav_16k_mono,
            load_wav_segment_16k_mono,
            vad_temporal_features,
        )
        from homemind_audio.realtime import (  # type: ignore
            HybridAudioPredictor,
            iter_waveform_windows,
        )
    return (
        load_wav_16k_mono,
        load_wav_segment_16k_mono,
        vad_temporal_features,
        HybridAudioPredictor,
        iter_waveform_windows,
    )


def run_wav(
    wav: str | Path,
    *,
    activity_model: str | Path = DEFAULT_ACTIVITY_MODEL,
    sound_model: str | Path = DEFAULT_SOUND_MODEL,
    start_seconds: float | None = None,
    end_seconds: float | None = None,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
    trace_id: str,
    base_time: datetime | None = None,
    defaults: FusionInputDefaults | None = None,
    max_windows: int | None = None,
    trailing_silence_seconds: float = 0.0,
    engine: ContextFusionEngine | None = None,
) -> Iterator[AudioFusionFrame]:
    (
        load_wav,
        load_segment,
        vad_features,
        predictor_type,
        iter_windows,
    ) = _import_audio_context()
    audio_path = Path(wav)
    if trailing_silence_seconds < 0:
        raise ValueError("trailing_silence_seconds cannot be negative")
    if (start_seconds is None) != (end_seconds is None):
        raise ValueError("start_seconds and end_seconds must be provided together")
    waveform = (
        load_segment(audio_path, start_seconds, end_seconds)
        if start_seconds is not None and end_seconds is not None
        else load_wav(audio_path)
    )
    if trailing_silence_seconds:
        import numpy as np

        waveform = np.concatenate(
            [
                waveform,
                np.zeros(int(round(16_000 * trailing_silence_seconds)), dtype=np.float32),
            ]
        )
    predictor = predictor_type(Path(activity_model), Path(sound_model))
    runner = AudioFusionRunner(
        predictor,
        lambda window: float(vad_features(window)[0]),
        engine=engine,
    )
    return runner.process_windows(
        iter_windows(
            waveform,
            window_seconds=window_seconds,
            hop_seconds=hop_seconds,
        ),
        trace_id=trace_id,
        base_time=base_time or datetime.now(timezone.utc),
        hop_seconds=hop_seconds,
        window_seconds=window_seconds,
        defaults=defaults,
        max_windows=max_windows,
    )
