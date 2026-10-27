import pytest

from homemind_stt.events import transcript_event, validate_transcript_event


def test_local_only_event_contract():
    value = transcript_event(
        "거실 불을", utterance_id="u-1", sequence=0,
        is_final=False, model_version="test",
    )
    assert value["scope"] == "RPI5_ONLY"
    assert value["source_node_id"] == "NODE-RPI5-02"
    assert value["microphone_id"] == "MIC-INDOOR-01"


def test_raw_audio_cannot_cross_contract():
    value = transcript_event(
        "창문 닫아줘", utterance_id="u-1", sequence=1,
        is_final=True, model_version="test",
    )
    value["raw_audio"] = "forbidden"
    with pytest.raises(ValueError, match="unknown fields"):
        validate_transcript_event(value)
