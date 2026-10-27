import json
from urllib.error import URLError

from homemind_exaone.client import ExaoneClient


def response(content):
    return {"choices": [{"message": {"content": content}}]}


def valid_light_json():
    return json.dumps(
        {
            "intent": "DEVICE_CONTROL",
            "device": "LIGHT",
            "action": "TURN_ON",
            "parameters": {},
            "ambiguous": False,
            "safety_flags": [],
            "clarification_question": None,
        },
        ensure_ascii=False,
    )


class QueueClient(ExaoneClient):
    def __init__(self, replies, **kwargs):
        super().__init__(**kwargs)
        self.replies = list(replies)
        self.payloads = []

    def _request_json(self, path, *, payload=None):
        self.payloads.append((path, payload))
        item = self.replies.pop(0)
        if isinstance(item, Exception):
            raise item
        return item


def test_request_uses_strict_json_schema_and_disables_thinking():
    client = QueueClient([response(valid_light_json())], retries=0)
    result = client.interpret("에어컨 세기를 높여줘")

    assert result.model_output_valid is True
    assert result.accepted is True
    assert result.origin == "model"
    payload = client.payloads[0][1]
    assert payload["response_format"]["type"] == "json_schema"
    assert payload["response_format"]["json_schema"]["strict"] is True
    assert payload["chat_template_kwargs"]["enable_thinking"] is False
    assert payload["temperature"] == 0.0


def test_invalid_output_is_retried_with_validation_feedback():
    invalid = json.dumps({"intent": "DEVICE_CONTROL"})
    client = QueueClient(
        [response(invalid), response(valid_light_json())], retries=1
    )

    result = client.interpret("에어컨 세기를 높여줘")

    assert result.model_output_valid is True
    assert result.attempts == 2
    second_user_message = json.loads(client.payloads[1][1]["messages"][-1]["content"])
    assert second_user_message["previous_output_errors"]


def test_markdown_wrapped_json_is_rejected_and_fails_closed():
    client = QueueClient(
        [response(f"```json\n{valid_light_json()}\n```")], retries=0
    )

    result = client.interpret("에어컨 세기를 높여줘")

    assert result.model_output_valid is False
    assert result.accepted is False
    assert result.origin == "fallback"
    assert result.proposal["intent"] == "CLARIFY"
    assert result.proposal["safety_flags"] == ["MODEL_OUTPUT_INVALID"]


def test_server_failure_returns_non_actionable_fallback():
    client = QueueClient([URLError("offline")], retries=1)

    result = client.interpret("에어컨 세기를 높여줘")

    assert result.model_output_valid is False
    assert result.accepted is False
    assert result.attempts == 1
    assert result.proposal["device"] is None
    assert result.proposal["action"] is None
    assert result.proposal["safety_flags"] == ["MODEL_UNAVAILABLE"]


def test_raw_model_evaluation_can_disable_guardrails_and_few_shots():
    client = QueueClient(
        [response(valid_light_json())],
        retries=0,
        enable_guardrails=False,
        include_few_shots=False,
    )
    result = client.interpret("불 켜줘")
    payload = client.payloads[0][1]
    assert result.origin == "model"
    assert len(payload["messages"]) == 2


def test_clear_command_calls_model_but_deterministic_result_wins_disagreement():
    wrong = json.dumps(
        {
            "intent": "DEVICE_CONTROL",
            "device": "TV",
            "action": "TURN_ON",
            "parameters": {},
            "ambiguous": False,
            "safety_flags": [],
            "clarification_question": None,
        },
        ensure_ascii=False,
    )
    client = QueueClient([response(wrong)], retries=0)
    result = client.interpret("거실 불을 켜 줘")
    assert len(client.payloads) == 1
    assert result.origin == "model_guard"
    assert (result.proposal["device"], result.proposal["action"]) == (
        "LIGHT",
        "TURN_ON",
    )
    assert "deterministic correction" in result.errors[0]
    user_data = json.loads(client.payloads[0][1]["messages"][-1]["content"])
    assert user_data["validated_local_candidate"]["device"] == "LIGHT"


def test_clear_command_uses_deterministic_candidate_when_model_is_offline():
    client = QueueClient([URLError("offline")], retries=0)
    result = client.interpret("창문 열어 줘")
    assert result.accepted is True
    assert result.origin == "guard_fallback"
    assert (result.proposal["device"], result.proposal["action"]) == (
        "WINDOW",
        "OPEN",
    )
