from homemind_stt.backends import Transcript
from homemind_stt.cascade import cascade_events


class FakeBackend:
    def __init__(self, result):
        self.result = result

    def transcribe(self, _wav):
        return self.result


def test_cascade_discards_partial_models_final_hypothesis():
    partial = FakeBackend(Transcript("틀린 최종문", "vosk", ("거실", "거실 불을")))
    final = FakeBackend(Transcript("거실 불을 켜줘", "whisper-small"))
    events = list(cascade_events(partial, final, "unused.wav"))
    assert [event["sequence"] for event in events] == [0, 1, 2]
    assert len({event["utterance_id"] for event in events}) == 1
    assert [event["is_final"] for event in events] == [False, False, True]
    assert events[-1]["text"] == "거실 불을 켜줘"
    assert events[-1]["model_version"] == "whisper-small"
