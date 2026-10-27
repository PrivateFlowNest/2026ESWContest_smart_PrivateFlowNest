from datetime import datetime, timedelta, timezone

from jsonschema import ValidationError
import pytest

from homemind_context_fusion.engine import ContextFusionEngine
from homemind_context_fusion.models import ContextObservation
from homemind_context_fusion.validation import validate_event, validate_sanitized_context


BASE_TIME = datetime(2026, 9, 3, 0, 0, tzinfo=timezone.utc)


def meeting(second: int) -> ContextObservation:
    return ContextObservation(
        timestamp=BASE_TIME + timedelta(seconds=second),
        trace_id=f"trace-{second}",
        audio_context="IN_PERSON_MEETING",
        audio_confidence=0.9,
        speech_ratio=0.6,
        turns_per_minute=5,
        duration_seconds=30,
        presence_zone="DESK",
        presence_confidence=0.95,
        tv_on=False,
        laptop_active=True,
    )


def quiet(second: int) -> ContextObservation:
    return ContextObservation(
        timestamp=BASE_TIME + timedelta(seconds=second),
        trace_id=f"trace-{second}",
        audio_context="QUIET",
        audio_confidence=0.96,
        speech_ratio=0.01,
        presence_zone="COMMON",
        presence_confidence=0.95,
        tv_on=False,
    )


def test_quiet_required_activates_only_after_three_updates():
    engine = ContextFusionEngine()
    first = engine.update(meeting(0))
    second = engine.update(meeting(2))
    third = engine.update(meeting(4))

    assert [first["quiet_required"], second["quiet_required"], third["quiet_required"]] == [
        False,
        False,
        True,
    ]
    assert third["context_type"] == "IN_PERSON_MEETING"
    assert third["activity"] == "MEETING"
    assert third["transition"] == "ACTIVATED"


def test_quiet_required_releases_after_two_non_meeting_updates():
    engine = ContextFusionEngine()
    for second in (0, 2, 4):
        engine.update(meeting(second))

    first = engine.update(quiet(6))
    second = engine.update(quiet(8))

    assert first["quiet_required"] is True
    assert first["confidence"] >= 0.8
    assert first["transition"] == "UNCHANGED"
    assert second["quiet_required"] is False
    assert second["context_type"] == "QUIET"
    assert second["transition"] == "DEACTIVATED"


def test_event_and_exaone_projection_validate():
    engine = ContextFusionEngine(ttl_seconds=10)
    event = engine.update(quiet(0))
    validate_event(event)
    projection = engine.sanitized_context(event)
    validate_sanitized_context(projection)

    assert event["trace_id"] == "trace-0"
    assert event["valid_until"] == "2026-09-03T00:00:10.000Z"
    assert set(projection) == {
        "presence_zone",
        "activity",
        "audio_context",
        "quiet_required",
        "confidence",
    }


def test_contract_cannot_carry_a_device_command():
    event = ContextFusionEngine().update(quiet(0))
    event["action"] = "LIGHT_ON"
    with pytest.raises(ValidationError):
        validate_event(event)


def test_stale_state_expires_before_new_evidence():
    engine = ContextFusionEngine(ttl_seconds=10)
    for second in (0, 2, 4):
        assert engine.update(meeting(second))["quiet_required"] is (second == 4)

    event = engine.update(meeting(30))
    assert event["context_type"] == "UNKNOWN"
    assert event["quiet_required"] is False
    assert "state:expired" in event["evidence"]


def test_out_of_order_observation_is_rejected():
    engine = ContextFusionEngine()
    engine.update(quiet(4))
    with pytest.raises(ValueError, match="ordered"):
        engine.update(quiet(2))


@pytest.mark.parametrize(
    ("zone", "activity"), [("BED", "SLEEPING"), ("DESK", "STUDYING")]
)
def test_quiet_person_at_bed_or_desk_activates_quiet_required(zone, activity):
    engine = ContextFusionEngine()
    events = []
    for second in (0, 2, 4):
        events.append(
            engine.update(
                ContextObservation(
                    timestamp=BASE_TIME + timedelta(seconds=second),
                    audio_context="QUIET",
                    audio_confidence=0.95,
                    speech_ratio=0.0,
                    presence_zone=zone,
                    presence_confidence=0.9,
                )
            )
        )
    assert events[-1]["context_type"] == "QUIET"
    assert events[-1]["activity"] == activity
    assert events[-1]["quiet_required"] is True
    assert events[-1]["transition"] == "ACTIVATED"
    assert f"policy:{activity.lower()}_requires_quiet" in events[-1]["evidence"]
