"""Bridge stabilized audio context events to EXAONE and the Pi 4 policy boundary."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
from time import monotonic
from typing import Any, Callable, Mapping
from urllib.request import Request, urlopen
from uuid import uuid4

from .client import ExaoneClient, InterpretationResult


MEETING_CONTEXTS = frozenset(
    {"ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING"}
)
ACTIVITY_CONTEXTS = frozenset(
    {
        "QUIET",
        "SELF_TALK",
        "TV_MEDIA",
        *MEETING_CONTEXTS,
        "UNKNOWN",
    }
)
_BASE_FIELDS = {
    "schema_version",
    "event_id",
    "trace_id",
    "timestamp",
    "source",
    "event_type",
    "confidence",
    "model_version",
    "evidence",
}
_ACTIVITY_FIELDS = _BASE_FIELDS | {"context_type", "policy_tags", "valid_until"}
_SOUND_FIELDS = _BASE_FIELDS | {"sound_type"}
_FUSED_ACTIVITY_FIELDS = _BASE_FIELDS | {
    "context_type",
    "presence_zone",
    "activity",
    "quiet_required",
    "transition",
    "valid_until",
}


class AudioEventValidationError(ValueError):
    """Raised when an event does not match the privacy-minimized audio contract."""


class ProposalDeliveryError(RuntimeError):
    """Raised when a validated proposal cannot be handed to Pi 4."""


@dataclass(frozen=True)
class BridgeOutcome:
    status: str
    transition: str | None = None
    envelope: dict[str, Any] | None = None
    errors: tuple[str, ...] = ()


def _parse_datetime(value: object, field: str) -> datetime:
    if not isinstance(value, str) or not value:
        raise AudioEventValidationError(f"{field} must be a non-empty date-time")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise AudioEventValidationError(f"{field} must be a valid date-time") from exc
    if parsed.tzinfo is None:
        raise AudioEventValidationError(f"{field} must include a timezone")
    return parsed


def validate_audio_event(event: Mapping[str, Any]) -> dict[str, Any]:
    """Validate the subset of the audio event contract consumed by this bridge."""

    if not isinstance(event, Mapping):
        raise AudioEventValidationError("audio event must be an object")
    value = dict(event)
    event_type = value.get("event_type")
    is_fused = value.get("source") == "pi5.context-fusion"
    if is_fused:
        allowed = _FUSED_ACTIVITY_FIELDS
        required = _FUSED_ACTIVITY_FIELDS
    else:
        allowed = _ACTIVITY_FIELDS if event_type == "ACTIVITY_CONTEXT" else _SOUND_FIELDS
        required = _ACTIVITY_FIELDS - {"evidence"} if event_type == "ACTIVITY_CONTEXT" else _SOUND_FIELDS - {"evidence"}
    if event_type not in {"ACTIVITY_CONTEXT", "SOUND_EVENT"}:
        raise AudioEventValidationError(f"unsupported event_type: {event_type!r}")
    missing = sorted(required - value.keys())
    unknown = sorted(value.keys() - allowed)
    if missing:
        raise AudioEventValidationError(f"missing fields: {', '.join(missing)}")
    if unknown:
        raise AudioEventValidationError(f"unknown fields: {', '.join(unknown)}")
    if value.get("schema_version") != "1.0":
        raise AudioEventValidationError("unsupported schema_version")
    if value.get("source") not in {"pi5.audio-context", "pi5.context-fusion"}:
        raise AudioEventValidationError("unsupported event source")
    for field in ("event_id", "trace_id", "model_version"):
        if not isinstance(value.get(field), str) or not value[field]:
            raise AudioEventValidationError(f"{field} must be a non-empty string")
    confidence = value.get("confidence")
    if isinstance(confidence, bool) or not isinstance(confidence, (int, float)):
        raise AudioEventValidationError("confidence must be a number")
    if not 0.0 <= float(confidence) <= 1.0:
        raise AudioEventValidationError("confidence must be between 0 and 1")
    _parse_datetime(value.get("timestamp"), "timestamp")
    evidence = value.get("evidence", [])
    if not isinstance(evidence, list) or not all(isinstance(item, str) for item in evidence):
        raise AudioEventValidationError("evidence must be a string array")
    if len(evidence) != len(set(evidence)):
        raise AudioEventValidationError("evidence must contain unique values")

    if event_type == "ACTIVITY_CONTEXT":
        if value.get("context_type") not in ACTIVITY_CONTEXTS:
            raise AudioEventValidationError("unsupported context_type")
        if is_fused:
            if not isinstance(value.get("quiet_required"), bool):
                raise AudioEventValidationError("quiet_required must be boolean")
            if value.get("presence_zone") not in {
                "BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"
            }:
                raise AudioEventValidationError("unsupported presence_zone")
            if value.get("activity") not in {
                "RESTING", "SLEEPING", "STUDYING", "WORKING", "MEETING",
                "BATHROOM_USE", "ENTERING", "LEAVING", "GENERAL_ACTIVITY", "UNKNOWN",
            }:
                raise AudioEventValidationError("unsupported activity")
            if value.get("transition") not in {
                "ACTIVATED", "UPDATED", "DEACTIVATED", "UNCHANGED"
            }:
                raise AudioEventValidationError("unsupported transition")
            eligible = (
                value["context_type"] in MEETING_CONTEXTS
                or value["activity"] in {"SLEEPING", "STUDYING"}
            )
            if value["quiet_required"] and not eligible:
                raise AudioEventValidationError(
                    "quiet_required lacks a meeting, sleeping, or studying reason"
                )
        else:
            tags = value.get("policy_tags")
            if not isinstance(tags, list) or any(tag != "QUIET_REQUIRED" for tag in tags):
                raise AudioEventValidationError("policy_tags contains an unsupported value")
            if len(tags) != len(set(tags)):
                raise AudioEventValidationError("policy_tags must contain unique values")
            if ("QUIET_REQUIRED" in tags) != (value["context_type"] in MEETING_CONTEXTS):
                raise AudioEventValidationError(
                    "QUIET_REQUIRED must match a stabilized meeting context"
                )
        _parse_datetime(value.get("valid_until"), "valid_until")
    elif value.get("sound_type") not in {"DOORBELL", "WASHER_COMPLETE", "ALARM", "OTHER"}:
        raise AudioEventValidationError("unsupported sound_type")
    return value


def activity_context_from_event(event: Mapping[str, Any], *, presence_zone: str) -> dict[str, Any]:
    if event.get("source") == "pi5.context-fusion":
        return {
            "presence_zone": event["presence_zone"],
            "activity": event["activity"],
            "audio_context": event["context_type"],
            "quiet_required": event["quiet_required"],
            "confidence": float(event["confidence"]),
        }
    context_type = str(event["context_type"])
    quiet_required = "QUIET_REQUIRED" in event["policy_tags"]
    return {
        "presence_zone": presence_zone,
        "activity": "MEETING" if quiet_required else "GENERAL_ACTIVITY",
        "audio_context": context_type,
        "quiet_required": quiet_required,
        "confidence": float(event["confidence"]),
    }


def _expected_transition(proposal: Mapping[str, Any], quiet_required: bool) -> bool:
    if quiet_required:
        expected = ("CONTEXT_SUGGESTION", "ALL_QUIET_DEVICES", "SET_QUIET_MODE")
    else:
        expected = ("DEVICE_CONTROL", "ALL_QUIET_DEVICES", "RESUME")
    actual = (proposal.get("intent"), proposal.get("device"), proposal.get("action"))
    return actual == expected


def _transition_utterance(context: Mapping[str, Any], quiet_required: bool) -> str:
    if not quiet_required:
        return "시스템 감지 이벤트: 조용한 환경이 필요했던 상황이 종료되어 자동 조용 모드를 해제해야 함."
    activity = context.get("activity")
    if activity == "SLEEPING":
        return "시스템 감지 이벤트: 수면 중이라 조용한 환경이 필요함."
    if activity == "STUDYING":
        return "시스템 감지 이벤트: 공부·집중 중이라 조용한 환경이 필요함."
    return "시스템 감지 이벤트: 회의가 시작되어 조용한 환경이 필요함."


def _envelope(
    *,
    event: Mapping[str, Any],
    context: Mapping[str, Any],
    result: InterpretationResult,
    transition: str,
) -> dict[str, Any]:
    return {
        "schema_version": "1.0",
        "message_type": "TASK_PROPOSAL",
        "proposal_id": f"proposal-{uuid4()}",
        "trace_id": event["trace_id"],
        "source_event_id": event["event_id"],
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "source": "pi5.exaone-audio-bridge",
        "transition": transition,
        "proposal": result.proposal,
        "model": {
            "name": result.model,
            "origin": result.origin,
            "latency_ms": round(result.latency_ms, 3),
        },
        "context": dict(context),
    }


class HttpProposalSink:
    """POST proposal envelopes to an explicitly configured Pi 4 endpoint."""

    def __init__(self, url: str, *, timeout_seconds: float = 10.0, api_key: str | None = None) -> None:
        if not url.startswith(("http://", "https://")):
            raise ValueError("Pi 4 URL must start with http:// or https://")
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        self.url = url
        self.timeout_seconds = float(timeout_seconds)
        self.api_key = api_key

    def __call__(self, envelope: Mapping[str, Any]) -> None:
        headers = {
            "Accept": "application/json",
            "Content-Type": "application/json; charset=utf-8",
            "Idempotency-Key": str(envelope["proposal_id"]),
        }
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        request = Request(
            self.url,
            data=json.dumps(envelope, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urlopen(request, timeout=self.timeout_seconds) as response:
                if not 200 <= response.status < 300:
                    raise ProposalDeliveryError(f"Pi 4 returned HTTP {response.status}")
        except OSError as exc:
            raise ProposalDeliveryError(f"Pi 4 delivery failed: {exc}") from exc


class AudioContextBridge:
    """Call EXAONE only on stable quiet-mode transitions and deliver safe proposals."""

    def __init__(
        self,
        client: ExaoneClient,
        sink: Callable[[Mapping[str, Any]], None],
        *,
        presence_zone: str = "DESK",
        retry_cooldown_seconds: float = 15.0,
        clock: Callable[[], float] = monotonic,
    ) -> None:
        if presence_zone not in {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"}:
            raise ValueError("unsupported presence_zone")
        if retry_cooldown_seconds < 0:
            raise ValueError("retry_cooldown_seconds must be non-negative")
        self.client = client
        self.sink = sink
        self.presence_zone = presence_zone
        self.retry_cooldown_seconds = float(retry_cooldown_seconds)
        self.clock = clock
        self._applied_quiet: bool | None = None
        self._pending: tuple[bool, dict[str, Any]] | None = None
        self._failed_target: bool | None = None
        self._next_attempt_at = 0.0

    def _deliver(self, quiet_required: bool, envelope: dict[str, Any], transition: str) -> BridgeOutcome:
        try:
            self.sink(envelope)
        except Exception as exc:
            self._pending = (quiet_required, envelope)
            self._next_attempt_at = self.clock() + self.retry_cooldown_seconds
            return BridgeOutcome(
                status="delivery_failed",
                transition=transition,
                envelope=envelope,
                errors=(str(exc),),
            )
        self._applied_quiet = quiet_required
        self._pending = None
        self._failed_target = None
        return BridgeOutcome(status="forwarded", transition=transition, envelope=envelope)

    def handle(self, raw_event: Mapping[str, Any]) -> BridgeOutcome:
        try:
            event = validate_audio_event(raw_event)
        except AudioEventValidationError as exc:
            return BridgeOutcome(status="invalid_event", errors=(str(exc),))
        if event["event_type"] != "ACTIVITY_CONTEXT":
            return BridgeOutcome(status="ignored_sound_event")

        quiet_required = (
            bool(event["quiet_required"])
            if event.get("source") == "pi5.context-fusion"
            else "QUIET_REQUIRED" in event["policy_tags"]
        )
        transition = "QUIET_MODE_ENTER" if quiet_required else "QUIET_MODE_EXIT"

        if self._applied_quiet is None and not quiet_required:
            self._applied_quiet = False
            return BridgeOutcome(status="baseline_initialized")
        if quiet_required == self._applied_quiet:
            if self._pending and self._pending[0] != quiet_required:
                self._pending = None
            return BridgeOutcome(status="ignored_heartbeat")
        if self.clock() < self._next_attempt_at:
            return BridgeOutcome(status="retry_cooldown", transition=transition)
        if self._pending and self._pending[0] == quiet_required:
            return self._deliver(quiet_required, self._pending[1], transition)

        context = activity_context_from_event(event, presence_zone=self.presence_zone)
        utterance = _transition_utterance(context, quiet_required)
        try:
            result = self.client.interpret(utterance, context=context)
        except Exception as exc:
            self._failed_target = quiet_required
            self._next_attempt_at = self.clock() + self.retry_cooldown_seconds
            return BridgeOutcome(
                status="model_error",
                transition=transition,
                errors=(str(exc),),
            )
        if not result.accepted:
            self._failed_target = quiet_required
            self._next_attempt_at = self.clock() + self.retry_cooldown_seconds
            return BridgeOutcome(
                status="model_rejected",
                transition=transition,
                errors=result.errors or ("EXAONE proposal was not accepted",),
            )
        if not _expected_transition(result.proposal, quiet_required):
            self._failed_target = quiet_required
            self._next_attempt_at = self.clock() + self.retry_cooldown_seconds
            return BridgeOutcome(
                status="transition_mismatch",
                transition=transition,
                errors=("EXAONE proposal does not match the detected transition",),
            )
        envelope = _envelope(
            event=event,
            context=context,
            result=result,
            transition=transition,
        )
        return self._deliver(quiet_required, envelope, transition)
