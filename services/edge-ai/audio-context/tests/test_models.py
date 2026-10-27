from datetime import datetime, timezone

import pytest

from homemind_audio.models import (
    Prediction,
    prediction_to_event,
    prediction_to_feature_event,
)


def test_meeting_prediction_adds_quiet_required_policy():
    prediction = Prediction(
        head="activity",
        label="IN_PERSON_MEETING",
        confidence=0.91,
        model_version="test-v1",
    )

    event = prediction_to_event(
        prediction,
        trace_id="trace-001",
        now=datetime(2026, 8, 23, 14, 0, tzinfo=timezone.utc),
    )

    assert event["event_type"] == "ACTIVITY_CONTEXT"
    assert event["context_type"] == "IN_PERSON_MEETING"
    assert event["policy_tags"] == ["QUIET_REQUIRED"]
    assert event["trace_id"] == "trace-001"


def test_doorbell_is_a_sound_event_without_context_policy():
    event = prediction_to_event(
        Prediction("sound_event", "DOORBELL", 0.98, "test-v1"),
        trace_id="trace-002",
    )

    assert event["event_type"] == "SOUND_EVENT"
    assert event["sound_type"] == "DOORBELL"
    assert "policy_tags" not in event


def test_invalid_head_label_pair_is_rejected():
    with pytest.raises(ValueError):
        Prediction("activity", "DOORBELL", 0.5, "test-v1")


def test_raw_activity_window_becomes_privacy_minimized_feature_event():
    event = prediction_to_feature_event(
        Prediction("activity", "ZOOM_SPEAKER", 0.87, "activity-v4"),
        trace_id="trace-feature",
        speech_ratio=0.42,
        duration_seconds=8.0,
        now=datetime(2026, 9, 3, 10, 0, tzinfo=timezone.utc),
        simulated=True,
    )

    assert event["event_type"] == "AUDIO_CONTEXT"
    assert event["source"] == "pi5.audio-context"
    assert event["simulated"] is True
    assert event["payload"] == {
        "context_type": "ZOOM_SPEAKER",
        "confidence": 0.87,
        "speech_ratio": 0.42,
        "duration_seconds": 8.0,
        "model_version": "activity-v4",
    }
    assert "raw_audio" not in event
