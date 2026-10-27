"""Synchronize asynchronous, privacy-minimized feature events.

The audio window is the fusion clock. Other producers publish independently and
their most recent non-expired values are joined when an AUDIO_CONTEXT event
arrives. This keeps missing hardware signals explicit instead of silently reusing
stale state.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Mapping

from .engine import ContextFusionEngine
from .models import ContextObservation
from .validation import validate_event, validate_feature_event


AUXILIARY_EVENT_TYPES = (
    "TURN_TAKING",
    "PRESENCE",
    "DEVICE_STATE",
)


def _timestamp(value: object) -> datetime:
    result = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    if result.tzinfo is None:
        result = result.replace(tzinfo=timezone.utc)
    return result.astimezone(timezone.utc)


@dataclass(frozen=True)
class StreamIngestResult:
    accepted: bool
    reason: str
    event_type: str
    context_event: dict[str, object] | None = None
    input_status: dict[str, str] | None = None

    def debug_record(self) -> dict[str, object]:
        value: dict[str, object] = {
            "accepted": self.accepted,
            "reason": self.reason,
            "event_type": self.event_type,
        }
        if self.input_status is not None:
            value["input_status"] = self.input_status
        if self.context_event is not None:
            value["context_event"] = self.context_event
        return value


class FeatureStreamFusion:
    """Join asynchronous feature observations and invoke context hysteresis."""

    def __init__(
        self,
        *,
        engine: ContextFusionEngine | None = None,
        minimum_turn_confidence: float = 0.50,
        minimum_presence_confidence: float = 0.50,
        duplicate_window: int = 4096,
    ) -> None:
        if not all(
            0.0 <= value <= 1.0
            for value in (
                minimum_turn_confidence,
                minimum_presence_confidence,
            )
        ):
            raise ValueError("minimum confidence values must be between 0 and 1")
        if duplicate_window < 1:
            raise ValueError("duplicate_window must be positive")
        self.engine = engine or ContextFusionEngine()
        self.minimum_turn_confidence = minimum_turn_confidence
        self.minimum_presence_confidence = minimum_presence_confidence
        self._latest: dict[str, dict[str, Any]] = {}
        self._seen_order: deque[str] = deque()
        self._seen_ids: set[str] = set()
        self._duplicate_window = duplicate_window

    def _remember(self, event_id: str) -> None:
        self._seen_ids.add(event_id)
        self._seen_order.append(event_id)
        while len(self._seen_order) > self._duplicate_window:
            self._seen_ids.discard(self._seen_order.popleft())

    def _fresh_event(
        self, event_type: str, anchor: datetime
    ) -> tuple[dict[str, Any] | None, str]:
        value = self._latest.get(event_type)
        if value is None:
            return None, "missing"
        timestamp = _timestamp(value["timestamp"])
        if timestamp > anchor:
            return None, "future"
        if _timestamp(value["valid_until"]) < anchor:
            return None, "stale"
        return value, "fresh"

    def ingest(self, value: Mapping[str, Any]) -> StreamIngestResult:
        event = dict(value)
        validate_feature_event(event)
        event_type = str(event["event_type"])
        event_id = str(event["event_id"])
        timestamp = _timestamp(event["timestamp"])
        valid_until = _timestamp(event["valid_until"])
        if valid_until <= timestamp:
            raise ValueError("valid_until must be later than timestamp")
        if event_id in self._seen_ids:
            return StreamIngestResult(False, "duplicate", event_type)

        previous = self._latest.get(event_type)
        if previous is not None and timestamp < _timestamp(previous["timestamp"]):
            self._remember(event_id)
            return StreamIngestResult(False, "out_of_order", event_type)

        self._remember(event_id)
        self._latest[event_type] = event
        if event_type != "AUDIO_CONTEXT":
            return StreamIngestResult(True, "buffered", event_type)

        input_status: dict[str, str] = {"AUDIO_CONTEXT": "fresh"}
        auxiliary: dict[str, dict[str, Any] | None] = {}
        for auxiliary_type in AUXILIARY_EVENT_TYPES:
            auxiliary[auxiliary_type], input_status[auxiliary_type] = self._fresh_event(
                auxiliary_type, timestamp
            )

        turn_event = auxiliary["TURN_TAKING"]
        turn_payload = turn_event["payload"] if turn_event else {}
        if turn_event and float(turn_payload["confidence"]) < self.minimum_turn_confidence:
            input_status["TURN_TAKING"] = "low_confidence"
            turns_per_minute = None
        else:
            turns_per_minute = (
                float(turn_payload["turns_per_minute"]) if turn_event else None
            )

        presence_event = auxiliary["PRESENCE"]
        presence_payload = presence_event["payload"] if presence_event else {}
        presence_confidence = (
            float(presence_payload["confidence"]) if presence_event else 0.0
        )
        if presence_event and presence_confidence < self.minimum_presence_confidence:
            input_status["PRESENCE"] = "low_confidence"
            presence_zone = "UNKNOWN"
        else:
            presence_zone = (
                str(presence_payload["presence_zone"])
                if presence_event
                else "UNKNOWN"
            )

        device_event = auxiliary["DEVICE_STATE"]
        device_payload = device_event["payload"] if device_event else {}
        audio_payload = event["payload"]
        observation = ContextObservation(
            audio_context=str(audio_payload["context_type"]),
            audio_confidence=float(audio_payload["confidence"]),
            audio_class_probabilities=audio_payload.get("class_probabilities"),
            speech_ratio=float(audio_payload["speech_ratio"]),
            turns_per_minute=turns_per_minute,
            turn_taking_method=(
                str(turn_payload["method"]) if turn_event else "UNKNOWN"
            ),
            duration_seconds=float(audio_payload["duration_seconds"]),
            presence_zone=presence_zone,
            presence_confidence=presence_confidence if presence_zone != "UNKNOWN" else 0.0,
            tv_on=device_payload.get("tv_on"),
            laptop_active=device_payload.get("laptop_active"),
            headphones_connected=device_payload.get("headphones_connected"),
            audio_model_version=str(audio_payload["model_version"]),
            trace_id=str(event["trace_id"]),
            timestamp=timestamp,
        )
        context_event = self.engine.update(observation)
        context_event["evidence"] = list(
            dict.fromkeys(
                [
                    *context_event["evidence"],
                    *(f"input:{name.lower()}:{status}" for name, status in input_status.items()),
                ]
            )
        )
        simulated_types = [
            name.lower()
            for name in ("AUDIO_CONTEXT", *AUXILIARY_EVENT_TYPES)
            if (item := (event if name == "AUDIO_CONTEXT" else auxiliary[name]))
            and bool(item["simulated"])
        ]
        if simulated_types:
            context_event["evidence"].append(
                "input:simulated:" + ",".join(simulated_types)
            )
        validate_event(context_event)
        return StreamIngestResult(
            True,
            "fused",
            event_type,
            context_event=context_event,
            input_status=input_status,
        )
