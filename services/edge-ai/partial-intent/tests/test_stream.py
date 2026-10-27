from homemind_partial_intent.runtime import IntentRuntime, StreamIntentGate


def test_stream_requires_stability_and_suppresses_duplicates(trained_baseline):
    _, model = trained_baseline
    gate = StreamIntentGate(IntentRuntime(model), confirmation_updates=2)
    first = gate.update("방 안이 점점 추워지는")
    second = gate.update("방 안이 점점 추워지는 것 같아")
    duplicate = gate.update("방 안이 점점 추워지는 것 같아 그래서 말인데")
    assert first["result"] == "INSUFFICIENT_EVIDENCE"
    assert second["result"] == "DETECTED"
    assert duplicate["result"] == "SUPPRESSED"
    assert all(event["execution_allowed"] is False for event in (first, second, duplicate))
