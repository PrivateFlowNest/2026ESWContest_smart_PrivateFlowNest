from datetime import datetime, timedelta, timezone

from jsonschema import ValidationError
import pytest

from homemind_context_fusion.stream_fusion import FeatureStreamFusion
from homemind_context_fusion.validation import validate_feature_event


LIVE_SOURCES = {
    "AUDIO_CONTEXT": ("pi5.audio-context", "NODE-RPI5-02"),
    "TURN_TAKING": ("pi5.turn-taking", "NODE-RPI5-02"),
    "PRESENCE": ("pi5.vision-presence", "NODE-RPI5-02"),
    "DEVICE_STATE": ("pi4.device-state", "NODE-RPI4-01"),
}
BASE = datetime(2026, 9, 3, 10, 0, tzinfo=timezone.utc)


def iso(second: int) -> str:
    return (BASE + timedelta(seconds=second)).isoformat().replace("+00:00", "Z")


def feature(
    event_type: str,
    second: int,
    payload: dict[str, object],
    *,
    event_id: str | None = None,
    ttl: int = 10,
    simulated: bool = True,
) -> dict[str, object]:
    source, source_node_id = (
        ("pc.scenario", "PC-SIMULATION") if simulated else LIVE_SOURCES[event_type]
    )
    return {
        "schema_version": "1.0",
        "event_id": event_id or f"{event_type.lower()}-{second}",
        "trace_id": "stream-test",
        "timestamp": iso(second),
        "valid_until": iso(second + ttl),
        "source": source,
        "source_node_id": source_node_id,
        "event_type": event_type,
        "simulated": simulated,
        "payload": payload,
    }


def audio(second: int, context: str = "IN_PERSON_MEETING") -> dict[str, object]:
    return feature(
        "AUDIO_CONTEXT",
        second,
        {
            "context_type": context,
            "confidence": 0.9,
            "speech_ratio": 0.6 if context != "QUIET" else 0.0,
            "duration_seconds": 4 + second,
            "model_version": "test-audio-v1",
        },
        ttl=5,
    )


def seed_meeting_auxiliary(fusion: FeatureStreamFusion, second: int = -1) -> None:
    values = [
        feature(
            "DEVICE_STATE",
            second,
            {
                "tv_on": False,
                "laptop_active": True,
                "headphones_connected": False,
            },
            ttl=20,
        ),
        feature(
            "PRESENCE",
            second,
            {"presence_zone": "DESK", "confidence": 0.9},
            ttl=20,
        ),
        feature(
            "TURN_TAKING",
            second,
            {
                "turns_per_minute": 5.0,
                "confidence": 0.8,
                "method": "SPEAKER_CHANGE",
            },
            ttl=20,
        ),
    ]
    for value in values:
        assert fusion.ingest(value).reason == "buffered"


def test_async_meeting_activates_and_quiet_releases():
    fusion = FeatureStreamFusion()
    seed_meeting_auxiliary(fusion)

    meeting_events = [fusion.ingest(audio(second)).context_event for second in (0, 2, 4)]
    assert [item["quiet_required"] for item in meeting_events] == [False, False, True]
    assert meeting_events[-1]["transition"] == "ACTIVATED"
    assert meeting_events[-1]["presence_zone"] == "DESK"
    assert "input:turn_taking:fresh" in meeting_events[-1]["evidence"]
    assert not any("direction" in item for item in meeting_events[-1]["evidence"])
    assert any(
        item.startswith("input:simulated:") for item in meeting_events[-1]["evidence"]
    )

    # The person leaves the desk when the meeting ends. DESK + QUIET is kept
    # quiet by policy because it can represent continued studying.
    fusion.ingest(
        feature(
            "PRESENCE",
            5,
            {"presence_zone": "COMMON", "confidence": 0.9},
            ttl=20,
        )
    )
    first_release = fusion.ingest(audio(6, "QUIET")).context_event
    second_release = fusion.ingest(audio(8, "QUIET")).context_event
    assert first_release["quiet_required"] is True
    assert second_release["quiet_required"] is False
    assert second_release["transition"] == "DEACTIVATED"


def test_expired_and_low_confidence_auxiliary_values_are_not_reused():
    fusion = FeatureStreamFusion()
    fusion.ingest(
        feature(
            "DEVICE_STATE",
            -3,
            {"tv_on": True, "laptop_active": False, "headphones_connected": False},
            ttl=2,
        )
    )
    fusion.ingest(
        feature(
            "PRESENCE",
            -1,
            {"presence_zone": "BED", "confidence": 0.2},
            ttl=20,
        )
    )

    result = fusion.ingest(audio(0))
    assert result.input_status["DEVICE_STATE"] == "stale"
    assert result.input_status["PRESENCE"] == "low_confidence"
    assert result.context_event["presence_zone"] == "UNKNOWN"
    assert "device:tv_on" not in result.context_event["evidence"]


def test_duplicate_and_out_of_order_per_stream_are_ignored():
    fusion = FeatureStreamFusion()
    recent = feature(
        "PRESENCE",
        2,
        {"presence_zone": "DESK", "confidence": 0.9},
        event_id="presence-recent",
    )
    assert fusion.ingest(recent).accepted is True
    duplicate = fusion.ingest(recent)
    assert duplicate.accepted is False
    assert duplicate.reason == "duplicate"

    older = feature(
        "PRESENCE",
        1,
        {"presence_zone": "COMMON", "confidence": 0.9},
        event_id="presence-older",
    )
    result = fusion.ingest(older)
    assert result.accepted is False
    assert result.reason == "out_of_order"


def test_future_auxiliary_event_is_not_applied_backwards():
    fusion = FeatureStreamFusion()
    fusion.ingest(
        feature(
            "DEVICE_STATE",
            5,
            {"tv_on": True, "laptop_active": False, "headphones_connected": False},
        )
    )
    result = fusion.ingest(audio(2))
    assert result.input_status["DEVICE_STATE"] == "future"
    assert "device:tv_on" not in result.context_event["evidence"]


def test_tv_state_suppresses_meeting_false_positive():
    fusion = FeatureStreamFusion()
    for value in (
        feature(
            "DEVICE_STATE",
            -1,
            {
                "tv_on": True,
                "laptop_active": False,
                "headphones_connected": False,
            },
        ),
        feature(
            "TURN_TAKING",
            -1,
            {
                "turns_per_minute": 1.0,
                "confidence": 0.7,
                "method": "SPEECH_BURST_PROXY",
            },
        ),
    ):
        fusion.ingest(value)

    first = fusion.ingest(audio(0)).context_event
    second = fusion.ingest(audio(2)).context_event
    assert first["quiet_required"] is False
    assert second["quiet_required"] is False
    assert second["context_type"] == "TV_MEDIA"
    assert "guard:tv_meeting_suppressed" in second["evidence"]


def test_feature_contract_rejects_raw_media_and_partial_device_snapshot():
    raw = audio(0)
    raw["payload"]["transcript"] = "private conversation"
    with pytest.raises(ValidationError):
        validate_feature_event(raw)

    partial = feature("DEVICE_STATE", 0, {"tv_on": True})
    with pytest.raises(ValidationError):
        validate_feature_event(partial)


def test_feature_contract_rejects_removed_source_direction():
    value = feature(
        "PRESENCE", 0, {"presence_zone": "DESK", "confidence": 0.9}
    )
    value["event_type"] = "SOURCE_DIRECTION"
    value["payload"] = {"direction": "TV", "confidence": 0.9}
    with pytest.raises(ValidationError):
        validate_feature_event(value)


@pytest.mark.parametrize(
    ("source", "source_node_id", "simulated"),
    [
        ("pc.scenario", "NODE-RPI5-02", True),
        ("pc.scenario", "PC-SIMULATION", False),
        ("pi5.audio-context", "PC-SIMULATION", False),
    ],
)
def test_feature_contract_enforces_source_node_id(source, source_node_id, simulated):
    value = audio(0)
    value["source"] = source
    value["source_node_id"] = source_node_id
    value["simulated"] = simulated
    with pytest.raises(ValidationError):
        validate_feature_event(value)
    del value["source_node_id"]
    with pytest.raises(ValidationError):
        validate_feature_event(value)


def test_live_feature_events_carry_hardware_node_id():
    fusion = FeatureStreamFusion()
    assert fusion.ingest(
        feature(
            "DEVICE_STATE",
            -1,
            {"tv_on": False, "laptop_active": True, "headphones_connected": False},
            simulated=False,
        )
    ).accepted
    result = fusion.ingest(feature("AUDIO_CONTEXT", 0, audio(0)["payload"], simulated=False))
    assert result.context_event["source_node_id"] == "NODE-RPI5-02"
    assert not any(
        item.startswith("input:simulated:") for item in result.context_event["evidence"]
    )


def test_validity_interval_must_move_forward():
    fusion = FeatureStreamFusion()
    value = audio(0)
    value["valid_until"] = value["timestamp"]
    with pytest.raises(ValueError, match="valid_until"):
        fusion.ingest(value)
