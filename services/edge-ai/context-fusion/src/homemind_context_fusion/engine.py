"""Stateful context fusion with activation/release hysteresis."""

from __future__ import annotations

from datetime import timedelta
import uuid

from .models import ContextObservation, QUIET_REQUIRED_CONTEXTS
from .scoring import Classification, classify


MODEL_VERSION = "context-fusion-rules-v1"


def _iso(value) -> str:
    return value.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _activity(context_type: str, zone: str) -> str:
    if context_type in QUIET_REQUIRED_CONTEXTS:
        return "MEETING"
    if zone == "BATHROOM":
        return "BATHROOM_USE"
    if zone == "BED":
        return "SLEEPING"
    if zone == "DESK":
        return "STUDYING" if context_type == "QUIET" else "WORKING"
    if zone == "ENTRANCE":
        return "ENTERING"
    if zone in {"COMMON", "OUTDOOR"}:
        return "GENERAL_ACTIVITY"
    return "UNKNOWN"


class ContextFusionEngine:
    def __init__(
        self,
        *,
        activation_updates: int = 3,
        release_updates: int = 2,
        change_updates: int = 2,
        ttl_seconds: float = 10.0,
        quiet_audio_confidence: float = 0.0,
        meeting_tv_margin: float = 0.0,
    ) -> None:
        if min(activation_updates, release_updates, change_updates) < 1:
            raise ValueError("hysteresis update counts must be positive")
        if ttl_seconds <= 0:
            raise ValueError("ttl_seconds must be positive")
        if not 0.0 <= quiet_audio_confidence <= 1.0:
            raise ValueError("quiet_audio_confidence must be between 0 and 1")
        if not 0.0 <= meeting_tv_margin <= 1.0:
            raise ValueError("meeting_tv_margin must be between 0 and 1")
        self.activation_updates = activation_updates
        self.release_updates = release_updates
        self.change_updates = change_updates
        self.ttl_seconds = ttl_seconds
        self.quiet_audio_confidence = quiet_audio_confidence
        self.meeting_tv_margin = meeting_tv_margin
        self.current_context = "UNKNOWN"
        self._current_quiet_required = False
        self._current_confidence = 0.25
        self._current_evidence = ["state:unconfirmed"]
        self._candidate = ("UNKNOWN", False)
        self._candidate_count = 0
        self._last_timestamp = None

    def _required_updates(self, predicted_quiet: bool) -> int:
        current_quiet = self._current_quiet_required
        if predicted_quiet and not current_quiet:
            return self.activation_updates
        if current_quiet and not predicted_quiet:
            return self.release_updates
        return self.change_updates

    def update(self, observation: ContextObservation) -> dict[str, object]:
        if self._last_timestamp is not None and observation.timestamp < self._last_timestamp:
            raise ValueError("observations must be ordered by timestamp")
        expired = bool(
            self._last_timestamp is not None
            and (observation.timestamp - self._last_timestamp).total_seconds() > self.ttl_seconds
        )
        if expired:
            self.current_context = "UNKNOWN"
            self._current_quiet_required = False
            self._current_confidence = 0.25
            self._current_evidence = ["state:expired"]
            self._candidate = ("UNKNOWN", False)
            self._candidate_count = 0

        result = classify(
            observation,
            quiet_audio_confidence=self.quiet_audio_confidence,
            meeting_tv_margin=self.meeting_tv_margin,
        )
        predicted_activity = _activity(result.context_type, observation.presence_zone)
        predicted_quiet = (
            result.context_type in QUIET_REQUIRED_CONTEXTS
            or (
                result.context_type == "QUIET"
                and predicted_activity in {"SLEEPING", "STUDYING"}
                and observation.presence_confidence >= 0.5
            )
        )
        predicted = (result.context_type, predicted_quiet)
        current = (self.current_context, self._current_quiet_required)
        previous_quiet = self._current_quiet_required
        required = self._required_updates(predicted_quiet)

        if predicted == current:
            self._candidate = predicted
            self._candidate_count = 0
        elif predicted != self._candidate:
            self._candidate = predicted
            self._candidate_count = 1
        else:
            self._candidate_count += 1

        changed = self._candidate != current and self._candidate_count >= required
        if changed:
            self.current_context, self._current_quiet_required = self._candidate
            self._candidate_count = 0

        if changed or predicted == (self.current_context, self._current_quiet_required):
            self._current_confidence = result.confidence
            self._current_evidence = list(result.evidence)
            if predicted_quiet and result.context_type == "QUIET":
                self._current_evidence.append(
                    f"policy:{predicted_activity.lower()}_requires_quiet"
                )

        was_quiet = previous_quiet
        now_quiet = self._current_quiet_required
        if changed and now_quiet and not was_quiet:
            transition = "ACTIVATED"
        elif changed and was_quiet and not now_quiet:
            transition = "DEACTIVATED"
        elif changed:
            transition = "UPDATED"
        else:
            transition = "UNCHANGED"

        confidence = self._current_confidence
        evidence = list(self._current_evidence)
        if (self.current_context, self._current_quiet_required) != predicted:
            evidence.extend(
                [
                    f"candidate:{result.context_type}:{self._candidate_count}/{required}",
                    *result.evidence,
                ]
            )
        if expired:
            evidence.insert(0, "state:expired")

        trace_id = observation.trace_id or f"trace-{uuid.uuid4()}"
        event = {
            "schema_version": "1.0",
            "event_id": f"ctx-{uuid.uuid4()}",
            "trace_id": trace_id,
            "timestamp": _iso(observation.timestamp),
            "source": "pi5.context-fusion",
            "source_node_id": "NODE-RPI5-02",
            "event_type": "ACTIVITY_CONTEXT",
            "context_type": self.current_context,
            "presence_zone": observation.presence_zone,
            "activity": _activity(self.current_context, observation.presence_zone),
            "quiet_required": now_quiet,
            "confidence": confidence,
            "evidence": list(dict.fromkeys(evidence)),
            "transition": transition,
            "valid_until": _iso(observation.timestamp + timedelta(seconds=self.ttl_seconds)),
            "model_version": MODEL_VERSION,
        }
        self._last_timestamp = observation.timestamp
        return event

    @staticmethod
    def sanitized_context(event: dict[str, object]) -> dict[str, object]:
        """Return the smaller payload that may be passed to EXAONE."""

        return {
            "presence_zone": event["presence_zone"],
            "activity": event["activity"],
            "audio_context": event["context_type"],
            "quiet_required": event["quiet_required"],
            "confidence": event["confidence"],
        }

    @staticmethod
    def classify_once(observation: ContextObservation) -> Classification:
        return classify(observation)
