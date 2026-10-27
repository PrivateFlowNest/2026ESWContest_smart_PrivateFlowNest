from homemind_stt.backends import Transcript
from homemind_stt.evaluation import evaluate, evaluate_acceptance


class FakeBackend:
    model_version = "fake"

    def transcribe(self, _path):
        return Transcript("거실 불을 켜줘", self.model_version, ("거실", "거실 불을"))


def test_report_omits_private_text_and_paths():
    report = evaluate(
        FakeBackend(),
        [{"id": "a", "audio_path": "ignored.wav", "reference": "거실 불을 켜줘", "duration_seconds": 1.0}],
    )
    assert report["character_error_rate"] == 0
    serialized = str(report)
    assert "거실 불을 켜줘" not in serialized
    assert "ignored.wav" not in serialized
    assert report["results"][0]["partial_update_count"] == 2
    assert report["keyword_group_metrics"]["light"] == {
        "hits": 1,
        "occurrences": 1,
        "recall": 1.0,
    }


def test_acceptance_reports_each_gate():
    report = evaluate(
        FakeBackend(),
        [{"id": "a", "audio_path": "ignored.wav", "reference": "거실 불을 켜줘", "duration_seconds": 1.0}],
    )
    result = evaluate_acceptance(
        report,
        {
            "character_error_rate_max": 0.25,
            "real_time_factor_max": 1.0,
            "homemind_keyword_recall_min": 0.90,
        },
    )
    assert result["passed"] is True
    assert all(result["checks"].values())
