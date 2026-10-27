"""Prediction models and conversion to the Pi 4 contract."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from uuid import uuid4

from .labels import HEAD_LABELS, QUIET_REQUIRED_CONTEXTS


@dataclass(frozen=True)
class Prediction:
    head: str
    label: str
    confidence: float
    model_version: str
    class_probabilities: dict[str, float] | None = None

    def __post_init__(self) -> None:
        if self.head not in HEAD_LABELS:
            raise ValueError(f"Unknown prediction head: {self.head}")
        if self.label not in HEAD_LABELS[self.head]:
            raise ValueError(f"Invalid label {self.label!r} for head {self.head!r}")
        if not 0.0 <= self.confidence <= 1.0:
            raise ValueError("confidence must be between 0 and 1")
        if self.class_probabilities is not None:
            unknown = set(self.class_probabilities) - set(HEAD_LABELS[self.head])
            if unknown:
                raise ValueError(f"Invalid probability labels: {sorted(unknown)}")
            if any(not 0.0 <= float(value) <= 1.0 for value in self.class_probabilities.values()):
                raise ValueError("class probabilities must be between 0 and 1")


def prediction_to_event(
    prediction: Prediction,
    *,
    trace_id: str,
    evidence: list[str] | None = None,
    now: datetime | None = None,
    context_ttl_seconds: int = 10,
) -> dict[str, object]:
    """Convert a model prediction into the schema consumed by Pi 4."""

    timestamp = now or datetime.now(timezone.utc)
    base: dict[str, object] = {
        "schema_version": "1.0",
        "event_id": f"evt-{uuid4()}",
        "trace_id": trace_id,
        "timestamp": timestamp.isoformat(),
        "source": "pi5.audio-context",
        "source_node_id": "NODE-RPI5-02",
        "confidence": round(float(prediction.confidence), 6),
        "model_version": prediction.model_version,
        "evidence": evidence or ["yamnet_embedding"],
    }

    if prediction.head == "activity":
        base.update(
            {
                "event_type": "ACTIVITY_CONTEXT",
                "context_type": prediction.label,
                "policy_tags": (
                    ["QUIET_REQUIRED"]
                    if prediction.label in QUIET_REQUIRED_CONTEXTS
                    else []
                ),
                "valid_until": (
                    timestamp + timedelta(seconds=context_ttl_seconds)
                ).isoformat(),
            }
        )
    else:
        base.update(
            {
                "event_type": "SOUND_EVENT",
                "sound_type": prediction.label,
            }
        )

    return base


def prediction_to_feature_event(
    prediction: Prediction,
    *,
    trace_id: str,
    speech_ratio: float,
    duration_seconds: float,
    now: datetime | None = None,
    ttl_seconds: int = 5,
    simulated: bool = False,
) -> dict[str, object]:
    """Convert one raw activity window to the context-fusion input contract."""

    if prediction.head != "activity":
        raise ValueError("only activity predictions can become AUDIO_CONTEXT features")
    if not 0.0 <= float(speech_ratio) <= 1.0:
        raise ValueError("speech_ratio must be between 0 and 1")
    if duration_seconds < 0 or ttl_seconds < 1:
        raise ValueError("duration_seconds must be non-negative and ttl positive")
    timestamp = now or datetime.now(timezone.utc)
    payload: dict[str, object] = {
        "context_type": prediction.label,
        "confidence": round(float(prediction.confidence), 6),
        "speech_ratio": round(float(speech_ratio), 6),
        "duration_seconds": round(float(duration_seconds), 3),
        "model_version": prediction.model_version,
    }
    if prediction.class_probabilities is not None:
        payload["class_probabilities"] = {
            label: round(float(probability), 6)
            for label, probability in prediction.class_probabilities.items()
        }
    return {
        "schema_version": "1.0",
        "event_id": f"audio-feature-{uuid4()}",
        "trace_id": trace_id,
        "timestamp": timestamp.isoformat(),
        "valid_until": (timestamp + timedelta(seconds=ttl_seconds)).isoformat(),
        "source": "pi5.audio-context",
        "source_node_id": "NODE-RPI5-02",
        "event_type": "AUDIO_CONTEXT",
        "simulated": bool(simulated),
        "payload": payload,
    }
