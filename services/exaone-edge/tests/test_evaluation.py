from homemind_exaone.client import InterpretationResult
from homemind_exaone.evaluation import check_targets, evaluate_cases


class FakeClient:
    model = "test-model"
    base_url = "http://test"

    def interpret(self, utterance, *, context=None):
        del utterance, context
        return InterpretationResult(
            proposal={
                "intent": "DEVICE_CONTROL",
                "device": "LIGHT",
                "action": "TURN_ON",
                "parameters": {},
                "ambiguous": False,
                "safety_flags": [],
                "clarification_question": None,
            },
            accepted=True,
            origin="model",
            model_output_valid=True,
            attempts=1,
            latency_ms=25.0,
            model=self.model,
            raw_content=None,
            errors=(),
        )


def test_evaluation_reports_schema_semantics_and_latency():
    report = evaluate_cases(
        FakeClient(),
        [
            {
                "id": "ok",
                "utterance": "불 켜줘",
                "expected": {"device": "LIGHT", "action": "TURN_ON"},
            },
            {
                "id": "wrong",
                "utterance": "TV 켜줘",
                "expected": {"device": "TV", "action": "TURN_ON"},
            },
        ],
    )

    assert report["contract_success_rate"] == 1.0
    assert report["model_acceptance_rate"] == 1.0
    assert report["model_schema_success_rate"] == 1.0
    assert report["model_first_pass_success_rate"] == 1.0
    assert report["guard_count"] == 0
    assert report["retry_recovered_count"] == 0
    assert report["expectation_success_rate"] == 0.5
    assert report["latency_ms"]["p95"] == 25.0
    assert report["category_metrics"]["uncategorized"]["total"] == 2


def test_array_valued_safety_flags_are_compared_as_an_exact_array():
    report = evaluate_cases(
        FakeClient(),
        [
            {
                "id": "empty-flags",
                "utterance": "불 켜줘",
                "expected": {
                    "device": "LIGHT",
                    "action": "TURN_ON",
                    "safety_flags": [],
                },
            }
        ],
    )

    assert report["expectation_success_rate"] == 1.0


def test_acceptance_targets_check_overall_and_category_metrics():
    report = {
        "contract_success_rate": 1.0,
        "expectation_success_rate": 0.96,
        "model_first_pass_success_rate": 0.98,
        "category_metrics": {
            "command": {
                "expectation_success_rate": 0.94,
                "contract_success_rate": 1.0,
            }
        },
    }
    failures = check_targets(
        report,
        {
            "overall": {"expectation_success_rate": 0.95},
            "categories": {"command": {"expectation_success_rate": 0.95}},
        },
    )
    assert failures == [
        "category command.expectation_success_rate: 0.94 < 0.95"
    ]


def test_clarification_question_is_scored_semantically_not_verbatim():
    class ClarifyingClient(FakeClient):
        def interpret(self, utterance, *, context=None):
            result = super().interpret(utterance, context=context)
            proposal = dict(result.proposal)
            proposal.update(
                intent="CLARIFY",
                device=None,
                action=None,
                ambiguous=True,
                safety_flags=["UNSUPPORTED_DEVICE"],
                clarification_question="지원하지 않는 기기입니다. 다른 기기를 말씀해 주세요.",
            )
            return InterpretationResult(**{**result.__dict__, "proposal": proposal})

    report = evaluate_cases(
        ClarifyingClient(),
        [
            {
                "utterance": "오븐 켜줘",
                "expected": {
                    "intent": "CLARIFY",
                    "device": None,
                    "action": None,
                    "ambiguous": True,
                    "safety_flags": ["UNSUPPORTED_DEVICE"],
                    "clarification_question": "오븐은 지원되지 않습니다.",
                },
            }
        ],
    )
    assert report["expectation_success_rate"] == 1.0
