"""Validated, privacy-minimized inputs for context fusion."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Mapping


CONTEXT_TYPES = (
    "QUIET",
    "SELF_TALK",
    "TV_MEDIA",
    "ZOOM_SPEAKER",
    "ZOOM_HEADPHONES",
    "IN_PERSON_MEETING",
    "UNKNOWN",
)
PRESENCE_ZONES = (
    "BED",
    "DESK",
    "BATHROOM",
    "ENTRANCE",
    "COMMON",
    "OUTDOOR",
    "UNKNOWN",
)
TURN_TAKING_METHODS = ("SPEAKER_CHANGE", "SPEECH_BURST_PROXY", "UNKNOWN")
QUIET_REQUIRED_CONTEXTS = frozenset(
    {"ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"}
)


def _probability(name: str, value: float) -> float:
    result = float(value)
    if not 0.0 <= result <= 1.0:
        raise ValueError(f"{name} must be between 0 and 1")
    return result


def _timestamp(value: str | datetime | None) -> datetime:
    if value is None:
        return datetime.now(timezone.utc)
    if isinstance(value, datetime):
        result = value
    else:
        result = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    if result.tzinfo is None:
        result = result.replace(tzinfo=timezone.utc)
    return result.astimezone(timezone.utc)


@dataclass(frozen=True)
class ContextObservation:
    """One fusion window. It deliberately excludes raw audio, images and text."""

    audio_context: str = "UNKNOWN"
    audio_confidence: float = 0.0
    audio_class_probabilities: Mapping[str, float] | None = None
    speech_ratio: float = 0.0
    turns_per_minute: float | None = None
    turn_taking_method: str = "UNKNOWN"
    duration_seconds: float = 0.0
    presence_zone: str = "UNKNOWN"
    presence_confidence: float = 0.0
    tv_on: bool | None = None
    laptop_active: bool | None = None
    headphones_connected: bool | None = None
    audio_model_version: str | None = None
    trace_id: str | None = None
    timestamp: datetime = field(default_factory=lambda: datetime.now(timezone.utc))

    def __post_init__(self) -> None:
        if self.audio_context not in CONTEXT_TYPES:
            raise ValueError(f"unsupported audio_context: {self.audio_context}")
        if self.presence_zone not in PRESENCE_ZONES:
            raise ValueError(f"unsupported presence_zone: {self.presence_zone}")
        if self.turn_taking_method not in TURN_TAKING_METHODS:
            raise ValueError(f"unsupported turn_taking_method: {self.turn_taking_method}")
        _probability("audio_confidence", self.audio_confidence)
        if self.audio_class_probabilities is not None:
            unknown = set(self.audio_class_probabilities) - set(CONTEXT_TYPES)
            if unknown:
                raise ValueError(f"unsupported audio probability labels: {sorted(unknown)}")
            for label, probability in self.audio_class_probabilities.items():
                _probability(f"audio_class_probabilities.{label}", probability)
        _probability("speech_ratio", self.speech_ratio)
        _probability("presence_confidence", self.presence_confidence)
        if self.turns_per_minute is not None and self.turns_per_minute < 0:
            raise ValueError("turns_per_minute cannot be negative")
        if self.duration_seconds < 0:
            raise ValueError("duration_seconds cannot be negative")
        for name in ("tv_on", "laptop_active", "headphones_connected"):
            value = getattr(self, name)
            if value is not None and not isinstance(value, bool):
                raise ValueError(f"{name} must be boolean or null")
        object.__setattr__(self, "timestamp", _timestamp(self.timestamp))

    @classmethod
    def from_mapping(cls, value: Mapping[str, Any]) -> "ContextObservation":
        forbidden = {
            "raw_audio",
            "audio_bytes",
            "image",
            "image_bytes",
            "frame",
            "transcript",
            "coordinates",
            "identity",
        }
        def keys(node: object) -> set[str]:
            if isinstance(node, Mapping):
                result = {str(key) for key in node}
                for child in node.values():
                    result.update(keys(child))
                return result
            if isinstance(node, (list, tuple)):
                result: set[str] = set()
                for child in node:
                    result.update(keys(child))
                return result
            return set()

        present = forbidden.intersection(keys(value))
        if present:
            raise ValueError(f"raw or identifying input is forbidden: {sorted(present)}")
        if "source_direction" in value or (
            isinstance(value.get("features"), Mapping)
            and "source_direction" in value["features"]
        ):
            raise ValueError(
                "source_direction was removed: the single USB microphone has no "
                "direction capability"
            )
        audio = value.get("audio", {})
        vision = value.get("vision", {})
        features = value.get("features", {})
        devices = value.get("device_state", {})
        if not all(isinstance(item, Mapping) for item in (audio, vision, features, devices)):
            raise ValueError("audio, vision, features, and device_state must be objects")
        turns = value.get("turns_per_minute", features.get("turns_per_minute"))
        return cls(
            audio_context=str(
                value.get("audio_context", audio.get("context_type", "UNKNOWN"))
            ),
            audio_confidence=float(
                value.get("audio_confidence", audio.get("confidence", 0.0))
            ),
            audio_class_probabilities=value.get(
                "audio_class_probabilities", audio.get("class_probabilities")
            ),
            speech_ratio=float(value.get("speech_ratio", features.get("speech_ratio", 0.0))),
            turns_per_minute=None if turns is None else float(turns),
            turn_taking_method=str(
                value.get(
                    "turn_taking_method",
                    features.get("turn_taking_method", "UNKNOWN"),
                )
            ),
            duration_seconds=float(
                value.get("duration_seconds", features.get("duration_seconds", 0.0))
            ),
            presence_zone=str(
                value.get("presence_zone", vision.get("presence_zone", vision.get("zone", "UNKNOWN")))
            ),
            presence_confidence=float(
                value.get("presence_confidence", vision.get("confidence", 0.0))
            ),
            tv_on=value.get("tv_on", devices.get("tv_on")),
            laptop_active=value.get("laptop_active", devices.get("laptop_active")),
            headphones_connected=value.get(
                "headphones_connected", devices.get("headphones_connected")
            ),
            audio_model_version=value.get(
                "audio_model_version", audio.get("model_version")
            ),
            trace_id=value.get("trace_id", audio.get("trace_id")),
            timestamp=_timestamp(value.get("timestamp", audio.get("timestamp"))),
        )
