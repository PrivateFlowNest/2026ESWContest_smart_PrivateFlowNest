from __future__ import annotations

import csv

import joblib
import numpy as np
from scipy.io import wavfile

from homemind_audio.dataset import AudioRecord
from homemind_audio.embedding_cache import CachedEmbeddingExtractor
from homemind_audio.training import (
    benchmark_batch_sizes,
    predict_file,
    train_from_manifest,
)


class FakeExtractor:
    def __init__(self):
        self.calls = 0

    def extract_file(self, path):
        self.calls += 1
        name = path.stem
        if "meeting" in name:
            return np.array([1.0, 0.0, 0.0], dtype=np.float32)
        if "tv" in name:
            return np.array([0.0, 1.0, 0.0], dtype=np.float32)
        if "doorbell" in name:
            return np.array([0.0, 0.0, 1.0], dtype=np.float32)
        return np.array([0.5, 0.5, 0.0], dtype=np.float32)


class FakeBatchExtractor:
    cache_identity = "fake-batch-v1"

    def __init__(self):
        self.batch_calls = 0

    @staticmethod
    def _vector(waveform):
        return np.array(
            [float(np.mean(waveform)), float(np.std(waveform)), 1.0],
            dtype=np.float32,
        )

    def extract_waveforms(self, waveforms):
        self.batch_calls += 1
        return np.stack([self._vector(waveform) for waveform in waveforms])

    def extract_file(self, path):
        _rate, waveform = wavfile.read(path)
        return self._vector(np.asarray(waveform, dtype=np.float32))


def test_large_cache_lookup_uses_sequential_scan(tmp_path):
    wav_path = tmp_path / "meeting.wav"
    wav_path.touch()
    cache = CachedEmbeddingExtractor(FakeExtractor(), tmp_path / "cache.sqlite3")
    try:
        cache.extract_file(wav_path)
        results = cache.lookup_many([(wav_path, None, None)] * 5_000)
    finally:
        cache.close()

    assert len(results) == 5_000
    assert all(vector is not None for _key, vector in results)


def test_train_and_predict_with_frozen_embeddings(tmp_path):
    rows = []
    for prefix, label in (
        ("meeting", "IN_PERSON_MEETING"),
        ("tv", "TV_MEDIA"),
    ):
        for index in range(3):
            wav_path = tmp_path / f"{prefix}_{index}.wav"
            wav_path.touch()
            rows.append([wav_path.name, "activity", label, "train"])

    manifest = tmp_path / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split"])
        writer.writerows(rows)

    output = tmp_path / "artifacts"
    summary = train_from_manifest(manifest, output, extractor=FakeExtractor())
    prediction = predict_file(
        tmp_path / "meeting_0.wav",
        output / "activity.joblib",
        extractor=FakeExtractor(),
    )

    assert summary["heads"]["activity"]["train_samples"] == 6
    assert prediction.label == "IN_PERSON_MEETING"


def test_embedding_cache_is_reused_across_training_runs(tmp_path):
    rows = []
    for prefix, label in (("meeting", "IN_PERSON_MEETING"), ("tv", "TV_MEDIA")):
        for index in range(2):
            wav_path = tmp_path / f"{prefix}_{index}.wav"
            wav_path.touch()
            rows.append([wav_path.name, "activity", label, "train"])
    manifest = tmp_path / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split"])
        writer.writerows(rows)

    cache = tmp_path / "features.sqlite3"
    first_extractor = FakeExtractor()
    first = train_from_manifest(
        manifest,
        tmp_path / "first",
        extractor=first_extractor,
        cache_path=cache,
    )
    second_extractor = FakeExtractor()
    second = train_from_manifest(
        manifest,
        tmp_path / "second",
        extractor=second_extractor,
        cache_path=cache,
    )

    assert first_extractor.calls == 4
    assert first["embedding_cache"]["misses"] == 4
    assert second_extractor.calls == 0
    assert second["embedding_cache"]["hits"] == 4


def test_validation_and_test_metrics_are_reported_separately(tmp_path):
    rows = []
    for prefix, label in (("meeting", "IN_PERSON_MEETING"), ("tv", "TV_MEDIA")):
        for split, count in (("train", 1), ("validation", 1), ("test", 2)):
            for index in range(count):
                wav_path = tmp_path / f"{prefix}_{split}_{index}.wav"
                wav_path.touch()
                rows.append([wav_path.name, "activity", label, split])
    manifest = tmp_path / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split"])
        writer.writerows(rows)

    summary = train_from_manifest(
        manifest,
        tmp_path / "artifacts",
        extractor=FakeExtractor(),
    )

    evaluations = summary["heads"]["activity"]["evaluations"]
    assert evaluations["validation"]["samples"] == 2
    assert evaluations["test"]["samples"] == 4


def test_classifier_tuning_uses_validation_and_records_selected_settings(tmp_path):
    rows = []
    for prefix, label in (("meeting", "IN_PERSON_MEETING"), ("tv", "TV_MEDIA")):
        for split in ("train", "validation"):
            for index in range(2):
                wav_path = tmp_path / f"{prefix}_{split}_{index}.wav"
                wav_path.touch()
                rows.append([wav_path.name, "activity", label, split])
    manifest = tmp_path / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split"])
        writer.writerows(rows)

    output = tmp_path / "artifacts"
    summary = train_from_manifest(
        manifest,
        output,
        extractor=FakeExtractor(),
        tune_classifier=True,
        classifier_c_values=(0.1, 1.0),
    )

    selection = summary["heads"]["activity"]["classifier_selection"]
    bundle = joblib.load(output / "activity.joblib")
    assert selection["enabled"] is True
    assert len(selection["candidates"]) == 4
    assert selection["selected"]["C"] in {0.1, 1.0}
    assert bundle["feature_version"] == "mean-vad-v1"


def test_cost_sensitive_activity_adds_weighted_candidates(tmp_path):
    rows = []
    for prefix, label in (("meeting", "IN_PERSON_MEETING"), ("tv", "TV_MEDIA")):
        for split in ("train", "validation"):
            for index in range(4):
                wav_path = tmp_path / f"{prefix}_{split}_{index}.wav"
                wav_path.touch()
                rows.append([wav_path.name, "activity", label, split])
    manifest = tmp_path / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split"])
        writer.writerows(rows)

    output = tmp_path / "artifacts"
    summary = train_from_manifest(
        manifest,
        output,
        extractor=FakeExtractor(),
        tune_classifier=True,
        classifier_c_values=(0.1,),
        cost_sensitive_activity=True,
        heads=("activity",),
    )

    selection = summary["heads"]["activity"]["classifier_selection"]
    assert selection["cost_sensitive_activity"] is True
    assert len(selection["candidates"]) == 5
    assert "sound_event" not in summary["heads"]
    assert joblib.load(output / "activity.joblib")["model_version"].endswith("0.7.0")

    recall_output = tmp_path / "recall-artifacts"
    recall_summary = train_from_manifest(
        manifest,
        recall_output,
        extractor=FakeExtractor(),
        tune_classifier=True,
        classifier_c_values=(0.1,),
        cost_sensitive_activity=True,
        activity_cost_profile="recall-buffer",
        heads=("activity",),
    )
    recall_selection = recall_summary["heads"]["activity"]["classifier_selection"]
    assert recall_selection["activity_cost_profile"] == "recall-buffer"
    assert len(recall_selection["candidates"]) == 1
    assert recall_selection["selected"]["class_weight"]["IN_PERSON_MEETING"] == 0.9
    assert recall_selection["selected"]["class_weight"]["TV_MEDIA"] == 1.1
    assert joblib.load(recall_output / "activity.joblib")["model_version"].endswith(
        "0.7.1-recall-buffer"
    )


def test_batch_prefetch_writes_cache_and_second_run_reuses_it(tmp_path):
    rows = []
    for prefix, label, value in (
        ("meeting", "IN_PERSON_MEETING", 10_000),
        ("tv", "TV_MEDIA", -10_000),
    ):
        for index in range(2):
            wav_path = tmp_path / f"{prefix}_{index}.wav"
            wavfile.write(wav_path, 16_000, np.full(16_000, value, dtype=np.int16))
            rows.append([wav_path.name, "activity", label, "train"])
    manifest = tmp_path / "manifest.csv"
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["path", "head", "label", "split"])
        writer.writerows(rows)

    cache = tmp_path / "features.sqlite3"
    first_extractor = FakeBatchExtractor()
    first = train_from_manifest(
        manifest,
        tmp_path / "first",
        extractor=first_extractor,
        cache_path=cache,
        batch_size=2,
        prefetch_workers=2,
    )
    second_extractor = FakeBatchExtractor()
    second = train_from_manifest(
        manifest,
        tmp_path / "second",
        extractor=second_extractor,
        cache_path=cache,
        batch_size=2,
        prefetch_workers=2,
    )

    assert first_extractor.batch_calls == 2
    assert first["embedding_cache"]["misses"] == 4
    assert second_extractor.batch_calls == 0
    assert second["embedding_cache"]["hits"] == 4


def test_batch_benchmark_requires_single_item_equivalence(tmp_path):
    records = []
    for index, value in enumerate((1000, 2000, 3000, 4000)):
        path = tmp_path / f"sample-{index}.wav"
        wavfile.write(path, 16_000, np.full(16_000, value, dtype=np.int16))
        records.append(AudioRecord(path, "activity", "SELF_TALK", "train"))

    report = benchmark_batch_sizes(
        records,
        FakeBatchExtractor(),
        batch_sizes=(1, 2, 4),
        sample_count=4,
        prefetch_workers=2,
    )

    assert report["selected_batch_size"] in {1, 2, 4}
    assert all(result["equivalent_to_single"] for result in report["results"])
    assert max(result["max_abs_difference"] for result in report["results"]) == 0.0
