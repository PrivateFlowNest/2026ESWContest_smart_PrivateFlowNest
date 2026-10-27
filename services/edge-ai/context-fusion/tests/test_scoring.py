from datetime import datetime, timezone

import pytest

from homemind_context_fusion.engine import ContextFusionEngine
from homemind_context_fusion.models import ContextObservation


@pytest.mark.parametrize(
    ("label", "extra"),
    [
        ("QUIET", {"speech_ratio": 0.01}),
        (
            "SELF_TALK",
            {"speech_ratio": 0.5, "turns_per_minute": 0.5},
        ),
        (
            "TV_MEDIA",
            {"speech_ratio": 0.6, "tv_on": True},
        ),
        (
            "ZOOM_SPEAKER",
            {
                "speech_ratio": 0.5,
                "turns_per_minute": 2.5,
                "presence_zone": "DESK",
                "presence_confidence": 0.9,
                "laptop_active": True,
                "headphones_connected": False,
            },
        ),
        (
            "ZOOM_HEADPHONES",
            {
                "speech_ratio": 0.4,
                "presence_zone": "DESK",
                "presence_confidence": 0.9,
                "laptop_active": True,
                "headphones_connected": True,
            },
        ),
        (
            "IN_PERSON_MEETING",
            {
                "speech_ratio": 0.6,
                "turns_per_minute": 5.0,
                "duration_seconds": 30,
                "presence_zone": "COMMON",
                "presence_confidence": 0.9,
            },
        ),
    ],
)
def test_all_six_classes_are_explainable(label, extra):
    observation = ContextObservation(audio_context=label, audio_confidence=0.93, **extra)
    result = ContextFusionEngine.classify_once(observation)

    assert result.context_type == label
    assert 0.0 <= result.confidence <= 1.0
    assert result.evidence
    assert "margin:" in result.evidence[-1]


def test_tv_guard_blocks_meeting_false_positive():
    result = ContextFusionEngine.classify_once(
        ContextObservation(
            audio_context="IN_PERSON_MEETING",
            audio_confidence=0.76,
            speech_ratio=0.7,
            turns_per_minute=6,
            duration_seconds=60,
            presence_zone="COMMON",
            presence_confidence=0.9,
            tv_on=True,
        )
    )

    assert result.context_type == "TV_MEDIA"
    assert "guard:tv_meeting_suppressed" in result.evidence


def test_vad_silence_overrides_spurious_meeting_prediction():
    result = ContextFusionEngine.classify_once(
        ContextObservation(
            audio_context="IN_PERSON_MEETING",
            audio_confidence=0.75,
            speech_ratio=0.0,
        )
    )
    assert result.context_type == "QUIET"
    assert "guard:no_speech_context_suppressed" in result.evidence


def test_low_confidence_quiet_required_audio_can_be_suppressed():
    observation = ContextObservation(
        audio_context="IN_PERSON_MEETING",
        audio_confidence=0.54,
        speech_ratio=0.5,
    )

    default = ContextFusionEngine.classify_once(observation)
    tuned = ContextFusionEngine(quiet_audio_confidence=0.60).update(observation)

    assert default.context_type == "IN_PERSON_MEETING"
    assert tuned["quiet_required"] is False
    assert any(
        item.startswith("guard:quiet_audio_below_threshold:")
        for item in tuned["evidence"]
    )


def test_speech_burst_proxy_is_not_treated_as_true_speaker_change():
    true_turns = ContextFusionEngine.classify_once(
        ContextObservation(
            audio_context="IN_PERSON_MEETING",
            audio_confidence=0.6,
            speech_ratio=0.6,
            turns_per_minute=5.0,
            turn_taking_method="SPEAKER_CHANGE",
        )
    )
    proxy = ContextFusionEngine.classify_once(
        ContextObservation(
            audio_context="IN_PERSON_MEETING",
            audio_confidence=0.6,
            speech_ratio=0.6,
            turns_per_minute=5.0,
            turn_taking_method="SPEECH_BURST_PROXY",
        )
    )

    assert true_turns.scores["IN_PERSON_MEETING"] > proxy.scores["IN_PERSON_MEETING"]
    assert any(item.startswith("turn_taking:proxy:") for item in proxy.evidence)


def test_close_tv_probability_suppresses_meeting_false_positive():
    event = ContextFusionEngine(
        activation_updates=1,
        meeting_tv_margin=0.15,
    ).update(
        ContextObservation(
            audio_context="IN_PERSON_MEETING",
            audio_confidence=0.52,
            audio_class_probabilities={
                "IN_PERSON_MEETING": 0.52,
                "TV_MEDIA": 0.43,
                "UNKNOWN": 0.05,
            },
            speech_ratio=0.6,
        )
    )

    assert event["quiet_required"] is False
    assert any(item.startswith("guard:meeting_tv_margin:") for item in event["evidence"])


def test_raw_media_and_transcript_are_rejected():
    with pytest.raises(ValueError, match="forbidden"):
        ContextObservation.from_mapping({"raw_audio": "base64", "transcript": "private"})


def test_nested_service_outputs_are_adapted():
    observation = ContextObservation.from_mapping(
        {
            "audio": {
                "context_type": "ZOOM_SPEAKER",
                "confidence": 0.91,
                "trace_id": "trace-nested",
                "timestamp": "2026-09-03T01:02:03Z",
            },
            "vision": {"zone": "DESK", "confidence": 0.88},
            "features": {
                "speech_ratio": 0.5,
                "turns_per_minute": 3.0,
                "duration_seconds": 40,
            },
            "device_state": {
                "tv_on": False,
                "laptop_active": True,
                "headphones_connected": False,
            },
        }
    )

    assert observation.audio_context == "ZOOM_SPEAKER"
    assert observation.presence_zone == "DESK"
    assert observation.trace_id == "trace-nested"


@pytest.mark.parametrize(
    "value",
    [
        {"source_direction": "TV"},
        {"features": {"speech_ratio": 0.5, "source_direction": "LAPTOP"}},
    ],
)
def test_removed_source_direction_input_is_rejected(value):
    with pytest.raises(ValueError, match="source_direction was removed"):
        ContextObservation.from_mapping(value)


def test_nested_raw_image_is_rejected():
    with pytest.raises(ValueError, match="forbidden"):
        ContextObservation.from_mapping({"vision": {"image": "base64"}})


def test_naive_timestamp_is_normalized_to_utc():
    value = ContextObservation(timestamp=datetime(2026, 9, 3, 12, 0, 0))
    assert value.timestamp.tzinfo == timezone.utc
