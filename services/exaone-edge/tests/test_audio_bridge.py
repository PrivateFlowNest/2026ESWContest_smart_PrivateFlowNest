from datetime import datetime, timedelta, timezone

from homemind_exaone.audio_bridge import AudioContextBridge, validate_audio_event
from homemind_exaone.client import InterpretationResult


def event(*, quiet, event_id="evt-1", context_type=None, event_type="ACTIVITY_CONTEXT"):
    now = datetime.now(timezone.utc)
    base = {
        "schema_version": "1.0",
        "event_id": event_id,
        "trace_id": "trace-1",
        "timestamp": now.isoformat(),
        "source": "pi5.audio-context",
        "event_type": event_type,
        "confidence": 0.93,
        "model_version": "audio-v4",
        "evidence": ["yamnet_embedding"],
    }
    if event_type == "ACTIVITY_CONTEXT":
        base.update(
            context_type=context_type or ("IN_PERSON_MEETING" if quiet else "QUIET"),
            policy_tags=["QUIET_REQUIRED"] if quiet else [],
            valid_until=(now + timedelta(seconds=10)).isoformat(),
        )
    else:
        base["sound_type"] = "DOORBELL"
    return base


def fused_event(*, quiet, event_id="ctx-1", context_type=None):
    now = datetime.now(timezone.utc)
    return {
        "schema_version": "1.0",
        "event_id": event_id,
        "trace_id": "trace-fused",
        "timestamp": now.isoformat(),
        "source": "pi5.context-fusion",
        "event_type": "ACTIVITY_CONTEXT",
        "context_type": context_type or ("IN_PERSON_MEETING" if quiet else "TV_MEDIA"),
        "presence_zone": "COMMON",
        "activity": "MEETING" if quiet else "GENERAL_ACTIVITY",
        "quiet_required": quiet,
        "confidence": 0.91,
        "evidence": ["audio:IN_PERSON_MEETING:0.90"],
        "transition": "ACTIVATED" if quiet else "DEACTIVATED",
        "valid_until": (now + timedelta(seconds=10)).isoformat(),
        "model_version": "context-fusion-rules-v1",
    }


def proposal(*, quiet):
    return {
        "intent": "CONTEXT_SUGGESTION" if quiet else "DEVICE_CONTROL",
        "device": "ALL_QUIET_DEVICES",
        "action": "SET_QUIET_MODE" if quiet else "RESUME",
        "parameters": {"reason": "회의 상황 감지" if quiet else "회의 상황 종료"},
        "ambiguous": False,
        "safety_flags": [],
        "clarification_question": None,
    }


class FakeClient:
    model = "exaone-test"

    def __init__(self, replies=None):
        self.calls = []
        self.replies = list(replies or [])

    def interpret(self, utterance, *, context):
        self.calls.append((utterance, context))
        item = self.replies.pop(0) if self.replies else proposal(quiet=context["quiet_required"])
        accepted = item is not None
        return InterpretationResult(
            proposal=item or {
                "intent": "CLARIFY",
                "device": None,
                "action": None,
                "parameters": {},
                "ambiguous": True,
                "safety_flags": ["MODEL_UNAVAILABLE"],
                "clarification_question": "다시 말해 주세요.",
            },
            accepted=accepted,
            origin="model" if accepted else "fallback",
            model_output_valid=accepted,
            attempts=1,
            latency_ms=12.5,
            model=self.model,
            raw_content=None,
            errors=() if accepted else ("offline",),
        )


def test_meeting_transition_calls_exaone_once_and_suppresses_heartbeats():
    client = FakeClient()
    delivered = []
    bridge = AudioContextBridge(client, delivered.append)

    first = bridge.handle(event(quiet=True, event_id="evt-enter"))
    heartbeat = bridge.handle(event(quiet=True, event_id="evt-heartbeat"))

    assert first.status == "forwarded"
    assert first.transition == "QUIET_MODE_ENTER"
    assert heartbeat.status == "ignored_heartbeat"
    assert len(client.calls) == 1
    assert len(delivered) == 1
    assert delivered[0]["proposal"]["action"] == "SET_QUIET_MODE"
    assert delivered[0]["context"]["activity"] == "MEETING"
    assert "raw_audio" not in delivered[0]["context"]


def test_meeting_exit_calls_exaone_and_emits_resume():
    client = FakeClient()
    delivered = []
    bridge = AudioContextBridge(client, delivered.append)

    bridge.handle(event(quiet=True, event_id="evt-enter"))
    outcome = bridge.handle(event(quiet=False, event_id="evt-exit"))

    assert outcome.status == "forwarded"
    assert outcome.transition == "QUIET_MODE_EXIT"
    assert len(client.calls) == 2
    assert delivered[-1]["proposal"]["action"] == "RESUME"
    assert delivered[-1]["context"]["quiet_required"] is False


def test_initial_non_meeting_is_only_a_baseline_and_sound_events_are_ignored():
    client = FakeClient()
    delivered = []
    bridge = AudioContextBridge(client, delivered.append)

    assert bridge.handle(event(quiet=False)).status == "baseline_initialized"
    assert bridge.handle(event(quiet=False, event_type="SOUND_EVENT")).status == "ignored_sound_event"
    assert client.calls == []
    assert delivered == []


def test_wrong_model_action_is_never_forwarded():
    wrong = proposal(quiet=True)
    wrong.update(device="LIGHT", action="TURN_ON", intent="DEVICE_CONTROL")
    client = FakeClient([wrong])
    delivered = []
    bridge = AudioContextBridge(client, delivered.append)

    outcome = bridge.handle(event(quiet=True))

    assert outcome.status == "transition_mismatch"
    assert delivered == []


def test_delivery_failure_reuses_same_idempotent_envelope_without_reasking_model():
    now = [0.0]
    attempts = []

    def sink(envelope):
        attempts.append(envelope)
        if len(attempts) == 1:
            raise OSError("Pi 4 offline")

    client = FakeClient()
    bridge = AudioContextBridge(
        client,
        sink,
        retry_cooldown_seconds=5,
        clock=lambda: now[0],
    )

    first = bridge.handle(event(quiet=True, event_id="evt-enter"))
    now[0] = 2.0
    cooldown = bridge.handle(event(quiet=True, event_id="evt-heartbeat-1"))
    now[0] = 6.0
    retried = bridge.handle(event(quiet=True, event_id="evt-heartbeat-2"))

    assert first.status == "delivery_failed"
    assert cooldown.status == "retry_cooldown"
    assert retried.status == "forwarded"
    assert len(client.calls) == 1
    assert attempts[0]["proposal_id"] == attempts[1]["proposal_id"]


def test_invalid_or_extra_audio_fields_are_rejected():
    invalid = event(quiet=True)
    invalid["raw_audio"] = "must-not-cross-boundary"

    outcome = AudioContextBridge(FakeClient(), lambda _: None).handle(invalid)

    assert outcome.status == "invalid_event"
    assert "unknown fields" in outcome.errors[0]


def test_quiet_tag_must_agree_with_meeting_context():
    invalid = event(quiet=True, context_type="TV_MEDIA")

    outcome = AudioContextBridge(FakeClient(), lambda _: None).handle(invalid)

    assert outcome.status == "invalid_event"
    assert "must match" in outcome.errors[0]


def test_audio_contract_validator_accepts_generated_shape():
    assert validate_audio_event(event(quiet=True))["context_type"] == "IN_PERSON_MEETING"


def test_fused_context_event_calls_exaone_and_preserves_minimized_context():
    client = FakeClient()
    delivered = []
    outcome = AudioContextBridge(client, delivered.append).handle(fused_event(quiet=True))

    assert outcome.status == "forwarded"
    assert delivered[0]["context"] == {
        "presence_zone": "COMMON",
        "activity": "MEETING",
        "audio_context": "IN_PERSON_MEETING",
        "quiet_required": True,
        "confidence": 0.91,
    }


def test_fused_tv_context_initializes_baseline_without_model_call():
    client = FakeClient()
    outcome = AudioContextBridge(client, lambda _: None).handle(fused_event(quiet=False))
    assert outcome.status == "baseline_initialized"
    assert client.calls == []


def test_fused_event_rejects_unknown_private_fields():
    value = fused_event(quiet=True)
    value["transcript"] = "must not cross boundary"
    outcome = AudioContextBridge(FakeClient(), lambda _: None).handle(value)
    assert outcome.status == "invalid_event"


def test_fused_sleep_context_can_request_quiet_without_meeting_label():
    value = fused_event(quiet=True, context_type="QUIET")
    value.update(presence_zone="BED", activity="SLEEPING")
    assert validate_audio_event(value)["quiet_required"] is True

    client = FakeClient()
    outcome = AudioContextBridge(client, lambda _: None).handle(value)
    assert outcome.status == "forwarded"
    assert "수면 중" in client.calls[0][0]
