"""Schema errors must never quote message values (an llm/request carries the transcript)."""

import pytest

from homemind_runtime import pfn


def test_schema_error_does_not_quote_the_transcript():
    document = pfn.llm_request(text="x", utterance_id=pfn.new_id(), stt_model_version="kb", stt_confidence=None,
                               parser_result="NO_MATCH", context=None, ttl_ms=1000)
    document["transcript"]["text"] = "비밀번호는 일이삼사" * 60  # > 500 characters -> maxLength error
    with pytest.raises(ValueError) as caught:
        pfn.validate(document, "llm-request.schema.json")
    message = str(caught.value)
    assert "transcript/text fails maxLength" in message
    assert "비밀번호" not in message
