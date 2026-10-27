from datetime import datetime, timedelta, timezone

import joblib
import numpy as np

from homemind_audio.models import Prediction
from homemind_audio.realtime import (
    ContextStabilizer,
    FusionConfig,
    HybridAudioPredictor,
    adapt_feature_vector,
    iter_waveform_windows,
)
from homemind_audio.yamnet import (
    FEATURE_VERSION_MEAN_VAD_V1,
    FEATURE_VERSION_TEMPORAL_V2,
    YAMNET_HANDLE,
)


class FixedPipeline:
    def __init__(self, classes, probabilities, feature_count):
        self.classes_ = np.asarray(classes)
        self.probabilities = np.asarray(probabilities, dtype=np.float64)
        self.n_features_in_ = feature_count

    def predict_proba(self, rows):
        return np.repeat(self.probabilities.reshape(1, -1), len(rows), axis=0)


class FakeTemporalExtractor:
    feature_version = FEATURE_VERSION_TEMPORAL_V2

    def __init__(self):
        self.calls = 0

    def extract_waveform(self, waveform):
        self.calls += 1
        return np.arange(3084, dtype=np.float32)


def _write_bundle(path, *, head, version, pipeline):
    joblib.dump(
        {
            "pipeline": pipeline,
            "head": head,
            "model_version": f"test-{head}",
            "yamnet_handle": YAMNET_HANDLE,
            "feature_version": version,
        },
        path,
    )


def test_temporal_features_can_feed_mean_vad_model():
    temporal = np.arange(3084, dtype=np.float32)
    adapted = adapt_feature_vector(
        temporal,
        source_version=FEATURE_VERSION_TEMPORAL_V2,
        target_version=FEATURE_VERSION_MEAN_VAD_V1,
    )

    assert adapted.shape == (1036,)
    np.testing.assert_array_equal(adapted[:1024], temporal[:1024])
    np.testing.assert_array_equal(adapted[-12:], temporal[-12:])


def test_hybrid_predictor_runs_shared_extractor_once(tmp_path):
    activity_path = tmp_path / "activity.joblib"
    sound_path = tmp_path / "sound.joblib"
    _write_bundle(
        activity_path,
        head="activity",
        version=FEATURE_VERSION_MEAN_VAD_V1,
        pipeline=FixedPipeline(
            ["IN_PERSON_MEETING", "TV_MEDIA"], [0.9, 0.1], 1036
        ),
    )
    _write_bundle(
        sound_path,
        head="sound_event",
        version=FEATURE_VERSION_TEMPORAL_V2,
        pipeline=FixedPipeline(["DOORBELL", "OTHER"], [0.2, 0.8], 3084),
    )
    extractor = FakeTemporalExtractor()
    predictor = HybridAudioPredictor(
        activity_path, sound_path, extractor=extractor
    )

    activity, sound = predictor.predict_waveform(np.zeros(16_000, dtype=np.float32))

    assert extractor.calls == 1
    assert activity.label == "IN_PERSON_MEETING"
    assert sound.label == "OTHER"


def test_quiet_required_enters_after_three_and_exits_after_two_windows():
    config = FusionConfig(
        enter_quiet_count=3,
        exit_quiet_count=2,
        heartbeat_seconds=30,
    )
    stabilizer = ContextStabilizer(config)
    started = datetime(2026, 9, 2, tzinfo=timezone.utc)
    meeting = Prediction("activity", "IN_PERSON_MEETING", 0.9, "activity-v3")
    tv = Prediction("activity", "TV_MEDIA", 0.9, "activity-v3")
    other = Prediction("sound_event", "OTHER", 0.99, "sound-v4")

    assert stabilizer.process(meeting, other, trace_id="t", now=started) == ()
    assert stabilizer.process(
        meeting, other, trace_id="t", now=started + timedelta(seconds=1)
    ) == ()
    entered = stabilizer.process(
        meeting, other, trace_id="t", now=started + timedelta(seconds=2)
    )
    assert entered[0]["context_type"] == "IN_PERSON_MEETING"
    assert entered[0]["policy_tags"] == ["QUIET_REQUIRED"]

    assert stabilizer.process(
        tv, other, trace_id="t", now=started + timedelta(seconds=3)
    ) == ()
    exited = stabilizer.process(
        tv, other, trace_id="t", now=started + timedelta(seconds=4)
    )
    assert exited[0]["context_type"] == "TV_MEDIA"
    assert exited[0]["policy_tags"] == []


def test_low_confidence_activity_becomes_unknown_instead_of_actionable():
    config = FusionConfig(
        activity_confidence=0.8,
        enter_quiet_count=2,
        other_transition_count=2,
    )
    stabilizer = ContextStabilizer(config)
    meeting = Prediction("activity", "IN_PERSON_MEETING", 0.7, "activity-v3")
    other = Prediction("sound_event", "OTHER", 0.99, "sound-v4")

    stabilizer.process(meeting, other, trace_id="t")
    events = stabilizer.process(meeting, other, trace_id="t")

    assert events == ()
    assert stabilizer.activity.label == "UNKNOWN"


def test_activity_heartbeat_refreshes_context_ttl():
    config = FusionConfig(other_transition_count=1, heartbeat_seconds=5)
    stabilizer = ContextStabilizer(config)
    started = datetime(2026, 9, 2, tzinfo=timezone.utc)
    quiet = Prediction("activity", "QUIET", 0.9, "activity-v3")
    other = Prediction("sound_event", "OTHER", 0.99, "sound-v4")

    first = stabilizer.process(quiet, other, trace_id="t", now=started)
    assert len(first) == 1
    assert stabilizer.process(
        quiet, other, trace_id="t", now=started + timedelta(seconds=4)
    ) == ()
    heartbeat = stabilizer.process(
        quiet, other, trace_id="t", now=started + timedelta(seconds=5)
    )
    assert heartbeat[0]["context_type"] == "QUIET"
    assert heartbeat[0]["timestamp"] != first[0]["timestamp"]


def test_sound_event_needs_confirmation_and_obeys_cooldown():
    config = FusionConfig(
        other_transition_count=10,
        sound_confirmation_count=2,
        sound_cooldown_seconds=8,
    )
    stabilizer = ContextStabilizer(config)
    started = datetime(2026, 9, 2, tzinfo=timezone.utc)
    quiet = Prediction("activity", "QUIET", 0.9, "activity-v3")
    alarm = Prediction("sound_event", "ALARM", 0.9, "sound-v4")

    assert stabilizer.process(quiet, alarm, trace_id="t", now=started) == ()
    emitted = stabilizer.process(
        quiet, alarm, trace_id="t", now=started + timedelta(seconds=1)
    )
    assert emitted[0]["sound_type"] == "ALARM"
    assert stabilizer.process(
        quiet, alarm, trace_id="t", now=started + timedelta(seconds=2)
    ) == ()
    repeated = stabilizer.process(
        quiet, alarm, trace_id="t", now=started + timedelta(seconds=9)
    )
    assert repeated[0]["sound_type"] == "ALARM"


def test_waveform_windows_overlap_and_pad_short_audio():
    waveform = np.arange(80_000, dtype=np.float32)
    windows = list(
        iter_waveform_windows(waveform, window_seconds=4.0, hop_seconds=1.0)
    )
    assert len(windows) == 2
    assert windows[0].shape == (64_000,)
    np.testing.assert_array_equal(windows[1][:10], waveform[16_000:16_010])

    short = list(
        iter_waveform_windows(
            np.ones(8_000, dtype=np.float32),
            window_seconds=1.0,
            hop_seconds=0.5,
        )
    )
    assert len(short) == 1
    assert short[0][-1] == 0.0
