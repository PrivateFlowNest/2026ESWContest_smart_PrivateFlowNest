import numpy as np

from homemind_partial_intent.dataset import load_rows
from homemind_partial_intent.runtime import IntentRuntime
from homemind_partial_intent.training import evaluate_model
from homemind_partial_intent.validation import validate_event


def test_exported_model_meets_synthetic_baseline(trained_baseline):
    data, model = trained_baseline
    report = evaluate_model(data, model)
    assert report["accuracy"] >= 0.90
    assert report["macro_f1"] >= 0.90
    assert report["gate"]["proactive_false_positive_rate"] <= 0.05


def test_final_pc_challenge_meets_accuracy_target(trained_baseline):
    data, model = trained_baseline
    final_set = data.parent / "acceptance-korean-v3-final.jsonl"
    report = evaluate_model(data, model, input_file=final_set)
    assert report["raw_model_accuracy"] >= 0.90
    assert report["accuracy"] >= 0.90
    assert report["stream_gate"]["positive_recall"] >= 0.90
    assert report["execution_safety"]["executable_false_positive_rate"] == 0.0


def test_runtime_matches_expected_contract(trained_baseline):
    _, model = trained_baseline
    runtime = IntentRuntime(model)
    event = runtime.predict("오늘 저녁에는 방이 좀 추울 것 같은데")
    validate_event(event)
    assert event["intent"] == "CLIMATE_COMFORT"
    assert event["execution_allowed"] is False
    assert "CHECK_TEMPERATURE" in event["safe_preparations"]
    assert "text" not in event


def test_onnx_probabilities_sum_to_one(trained_baseline):
    data, model = trained_baseline
    runtime = IntentRuntime(model)
    rows = load_rows(data, "test")
    probabilities = runtime.probabilities(rows[0]["text"])
    assert np.isclose(sum(probabilities.values()), 1.0, atol=1e-5)


def test_malformed_unicode_is_safely_suppressed(trained_baseline):
    _, model = trained_baseline
    event = IntentRuntime(model).predict("회의\udcec")
    validate_event(event)
    assert event["intent"] == "NO_ACTION"
    assert event["result"] == "SUPPRESSED"
    assert event["execution_allowed"] is False


def test_explicit_cancellation_overrides_model(trained_baseline):
    _, model = trained_baseline
    runtime = IntentRuntime(model)
    for text in ("거실 불은 켜지 마", "에어컨은 건드리지 말고 그대로 둬"):
        event = runtime.predict(text)
        validate_event(event)
        assert event["intent"] == "NO_ACTION"
        assert event["result"] == "SUPPRESSED"
