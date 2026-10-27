"""Privacy-scoped local transcript event construction and validation."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Any, Mapping
from uuid import uuid4


FIELDS = {
    "schema_version", "event_id", "utterance_id", "sequence", "timestamp",
    "source", "source_node_id", "event_type", "scope", "microphone_id", "text", "is_final",
    "model_version", "confidence", "expires_at",
}


def transcript_event(
    text: str,
    *,
    utterance_id: str,
    sequence: int,
    is_final: bool,
    model_version: str,
    confidence: float | None = None,
    microphone_id: str = "MIC-INDOOR-01",
    now: datetime | None = None,
) -> dict[str, Any]:
    current = now or datetime.now(timezone.utc)
    value = {
        "schema_version": "1.0",
        "event_id": f"asr-{uuid4()}",
        "utterance_id": utterance_id,
        "sequence": sequence,
        "timestamp": current.isoformat(),
        "source": "pi5.speech-to-text",
        "source_node_id": "NODE-RPI5-02",
        "event_type": "ASR_UPDATE",
        "scope": "RPI5_ONLY",
        "microphone_id": microphone_id,
        "text": text,
        "is_final": is_final,
        "model_version": model_version,
        "confidence": confidence,
        "expires_at": (current + timedelta(seconds=30)).isoformat(),
    }
    return validate_transcript_event(value)


def validate_transcript_event(raw: Mapping[str, Any]) -> dict[str, Any]:
    value = dict(raw)
    missing = FIELDS - value.keys()
    unknown = value.keys() - FIELDS
    if missing:
        raise ValueError(f"missing fields: {', '.join(sorted(missing))}")
    if unknown:
        raise ValueError(f"unknown fields: {', '.join(sorted(unknown))}")
    constants = {
        "schema_version": "1.0",
        "source": "pi5.speech-to-text",
        "source_node_id": "NODE-RPI5-02",
        "event_type": "ASR_UPDATE",
        "scope": "RPI5_ONLY",
        "microphone_id": "MIC-INDOOR-01",
    }
    for field, expected in constants.items():
        if value[field] != expected:
            raise ValueError(f"{field} must be {expected!r}")
    for field in ("event_id", "utterance_id", "timestamp", "model_version", "expires_at"):
        if not isinstance(value[field], str) or not value[field]:
            raise ValueError(f"{field} must be a non-empty string")
    if not isinstance(value["sequence"], int) or isinstance(value["sequence"], bool) or value["sequence"] < 0:
        raise ValueError("sequence must be a non-negative integer")
    if not isinstance(value["text"], str) or len(value["text"]) > 500:
        raise ValueError("text must be a string of at most 500 characters")
    if not isinstance(value["is_final"], bool):
        raise ValueError("is_final must be boolean")
    confidence = value["confidence"]
    if confidence is not None and (
        isinstance(confidence, bool)
        or not isinstance(confidence, (int, float))
        or not 0 <= float(confidence) <= 1
    ):
        raise ValueError("confidence must be null or between 0 and 1")
    for field in ("timestamp", "expires_at"):
        parsed = datetime.fromisoformat(value[field].replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            raise ValueError(f"{field} must include timezone")
    return value
