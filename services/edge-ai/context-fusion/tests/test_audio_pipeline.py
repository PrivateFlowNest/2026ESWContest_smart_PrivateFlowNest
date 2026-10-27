from datetime import datetime, timezone
from types import SimpleNamespace

from homemind_context_fusion.audio_pipeline import (
    AudioFusionRunner,
    FusionInputDefaults,
    summarize_frames,
)


class FixedPredictor:
    def __init__(self, activity_label="IN_PERSON_MEETING", confidence=0.92):
        self.activity = SimpleNamespace(
            label=activity_label,
            confidence=confidence,
            model_version="fake-activity-v1",
        )
        self.sound = SimpleNamespace(
            label="OTHER",
            confidence=0.98,
            model_version="fake-sound-v1",
        )

    def predict_waveform(self, _window):
        return self.activity, self.sound


class SequencePredictor(FixedPredictor):
    def __init__(self):
        super().__init__()
        self.calls = 0

    def predict_waveform(self, _window):
        self.calls += 1
        if self.calls > 3:
            self.activity = SimpleNamespace(
                label="UNKNOWN",
                confidence=0.95,
                model_version="fake-activity-v1",
            )
        return self.activity, self.sound


def test_audio_windows_activate_quiet_required_without_manual_mode():
    runner = AudioFusionRunner(FixedPredictor(), lambda _window: 0.55)
    frames = list(
        runner.process_windows(
            [object(), object(), object()],
            trace_id="trace-audio-e2e",
            base_time=datetime(2026, 9, 3, tzinfo=timezone.utc),
            hop_seconds=1.0,
            window_seconds=4.0,
            defaults=FusionInputDefaults(
                presence_zone="DESK",
                presence_confidence=0.9,
                turns_per_minute=5.0,
            ),
        )
    )

    assert len(frames) == 3
    assert [item.context_event["quiet_required"] for item in frames] == [
        False,
        False,
        True,
    ]
    assert frames[-1].context_event["transition"] == "ACTIVATED"
    assert "audio_model:fake-activity-v1" in frames[-1].context_event["evidence"]
    assert frames[-1].activity_prediction["label"] == "IN_PERSON_MEETING"


def test_tv_device_guard_survives_audio_pipeline():
    runner = AudioFusionRunner(
        FixedPredictor("IN_PERSON_MEETING", confidence=0.80), lambda _window: 0.7
    )
    frames = list(
        runner.process_windows(
            [object(), object()],
            trace_id="trace-tv",
            base_time=datetime(2026, 9, 3, tzinfo=timezone.utc),
            hop_seconds=1.0,
            window_seconds=4.0,
            defaults=FusionInputDefaults(
                presence_zone="COMMON",
                presence_confidence=0.9,
                turns_per_minute=6.0,
                tv_on=True,
            ),
        )
    )

    assert frames[-1].context_event["context_type"] == "TV_MEDIA"
    assert frames[-1].context_event["quiet_required"] is False


def test_max_windows_stops_inference_early():
    runner = AudioFusionRunner(FixedPredictor("SELF_TALK"), lambda _window: 0.4)
    frames = list(
        runner.process_windows(
            [object(), object(), object()],
            trace_id="trace-limit",
            base_time=datetime(2026, 9, 3, tzinfo=timezone.utc),
            hop_seconds=1.0,
            window_seconds=4.0,
            max_windows=1,
        )
    )
    assert len(frames) == 1


def test_one_audio_stream_activates_and_releases_context():
    runner = AudioFusionRunner(
        SequencePredictor(), lambda window: 0.55 if window == "meeting" else 0.0
    )
    frames = list(
        runner.process_windows(
            ["meeting", "meeting", "meeting", "silence", "silence"],
            trace_id="trace-lifecycle",
            base_time=datetime(2026, 9, 3, tzinfo=timezone.utc),
            hop_seconds=1.0,
            window_seconds=4.0,
        )
    )
    assert frames[2].context_event["transition"] == "ACTIVATED"
    assert frames[2].context_event["quiet_required"] is True
    assert frames[4].context_event["transition"] == "DEACTIVATED"
    assert frames[4].context_event["quiet_required"] is False

    summary = summarize_frames(frames, window_seconds=4.0)
    assert summary["quiet_required_ever"] is True
    assert summary["transitions"][0]["decision_seconds"] == 6.0
    assert summary["privacy"]["raw_audio_persisted"] is False
