"""Train and run small classifier heads over frozen YAMNet embeddings."""

from __future__ import annotations

from collections import deque
from concurrent.futures import Future, ThreadPoolExecutor
import json
import sys
from pathlib import Path
from time import perf_counter
from typing import Callable, Mapping, Protocol

import joblib
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import (
    accuracy_score,
    classification_report,
    confusion_matrix,
    f1_score,
)
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler

from .audio import load_wav_16k_mono, load_wav_segment_16k_mono
from .dataset import AudioRecord, iter_manifest
from .embedding_cache import CachedEmbeddingExtractor
from .models import Prediction
from .yamnet import (
    FEATURE_VERSION_MEAN_VAD_V1,
    YAMNET_HANDLE,
    YamnetEmbeddingExtractor,
)


class EmbeddingExtractor(Protocol):
    def extract_file(self, path: str | Path) -> np.ndarray: ...


DEFAULT_CLASSIFIER_C_VALUES = (0.03, 0.1, 0.3, 1.0, 3.0, 10.0)


def _classifier_pipeline(
    *,
    c_value: float = 1.0,
    class_weight: str | Mapping[str, float] | None = "balanced",
) -> Pipeline:
    return Pipeline(
        [
            ("scale", StandardScaler()),
            (
                "classifier",
                LogisticRegression(
                    C=c_value,
                    max_iter=2_000,
                    class_weight=class_weight,
                    random_state=42,
                ),
            ),
        ]
    )


def _select_classifier(
    x_train: np.ndarray,
    y_train: np.ndarray,
    x_validation: np.ndarray,
    y_validation: np.ndarray,
    *,
    c_values: tuple[float, ...],
    progress: Callable[[str], None] | None,
    head: str,
    cost_sensitive_activity: bool = False,
    activity_cost_profile: str = "search",
) -> tuple[Pipeline, dict[str, object]]:
    """Select logistic-regression settings using validation data only."""

    candidates: list[dict[str, object]] = []
    fitted: list[Pipeline | None] = []
    settings: list[tuple[float, str | Mapping[str, float] | None]] = []
    if cost_sensitive_activity and head == "activity":
        labels = {str(label) for label in np.unique(y_train)}
        cost_profiles = (
            (0.90, 1.10),
            (0.80, 1.25),
            (0.70, 1.50),
            (0.60, 2.00),
            (0.50, 2.50),
        )
        if activity_cost_profile == "recall-buffer":
            cost_profiles = cost_profiles[:1]
        elif activity_cost_profile != "search":
            raise ValueError(
                "activity_cost_profile must be 'search' or 'recall-buffer'"
            )
        for meeting_weight, tv_weight in cost_profiles:
            weights = {label: 1.0 for label in labels}
            weights["IN_PERSON_MEETING"] = meeting_weight
            weights["TV_MEDIA"] = tv_weight
            settings.extend((float(c_value), weights) for c_value in c_values)
    else:
        settings = [
            (float(c_value), class_weight)
            for class_weight in ("balanced", None)
            for c_value in c_values
        ]
    for index, (c_value, class_weight) in enumerate(settings, start=1):
        pipeline = _classifier_pipeline(
            c_value=c_value,
            class_weight=class_weight,
        )
        candidate: dict[str, object] = {
            "C": c_value,
            "class_weight": class_weight,
        }
        try:
            pipeline.fit(x_train, y_train)
            predicted = pipeline.predict(x_validation)
            candidate.update(
                {
                    "validation_macro_f1": float(
                        f1_score(
                            y_validation,
                            predicted,
                            average="macro",
                            zero_division=0,
                        )
                    ),
                    "validation_accuracy": float(
                        accuracy_score(y_validation, predicted)
                    ),
                    "tv_media_as_in_person_meeting": int(
                        np.sum(
                            (y_validation == "TV_MEDIA")
                            & (predicted == "IN_PERSON_MEETING")
                        )
                    ),
                }
            )
            true_quiet = np.isin(
                y_validation, ("IN_PERSON_MEETING", "ZOOM_SPEAKER")
            )
            predicted_quiet = np.isin(
                predicted, ("IN_PERSON_MEETING", "ZOOM_SPEAKER")
            )
            candidate["quiet_recall"] = float(
                np.mean(predicted_quiet[true_quiet]) if np.any(true_quiet) else 0.0
            )
            candidate["quiet_false_positive_rate"] = float(
                np.mean(predicted_quiet[~true_quiet]) if np.any(~true_quiet) else 0.0
            )
            meeting_mask = y_validation == "IN_PERSON_MEETING"
            candidate["in_person_meeting_recall"] = float(
                np.mean(predicted[meeting_mask] == "IN_PERSON_MEETING")
                if np.any(meeting_mask)
                else 0.0
            )
            fitted.append(pipeline)
        except Exception as exc:
            candidate["error"] = f"{type(exc).__name__}: {exc}"
            fitted.append(None)
        candidates.append(candidate)
        if progress:
            score = candidate.get("validation_macro_f1", "error")
            weight_name = (
                json.dumps(class_weight, sort_keys=True)
                if isinstance(class_weight, Mapping)
                else class_weight or "none"
            )
            progress(
                f"{head} classifier tuning: {index}/{len(settings)} "
                f"C={c_value:g} class_weight={weight_name} macro_f1={score}"
            )

    valid_indices = [
        index
        for index, candidate in enumerate(candidates)
        if "validation_macro_f1" in candidate
    ]
    if not valid_indices:
        raise RuntimeError(f"Every classifier candidate failed for head {head!r}")
    constrained = [
        index
        for index in valid_indices
        if float(candidates[index]["quiet_recall"]) >= 0.90
        and float(candidates[index]["in_person_meeting_recall"]) >= 0.85
    ]
    if cost_sensitive_activity and head == "activity" and constrained:
        best_index = min(
            constrained,
            key=lambda index: (
                int(candidates[index]["tv_media_as_in_person_meeting"]),
                float(candidates[index]["quiet_false_positive_rate"]),
                -float(candidates[index]["validation_macro_f1"]),
                -float(candidates[index]["validation_accuracy"]),
            ),
        )
        selection_metric = (
            "minimum TV_MEDIA→IN_PERSON_MEETING errors and quiet false-positive "
            "rate subject to quiet recall >=0.90 and meeting recall >=0.85"
        )
    else:
        best_index = max(
            valid_indices,
            key=lambda index: (
                float(candidates[index]["validation_macro_f1"]),
                float(candidates[index]["validation_accuracy"]),
                -int(candidates[index]["tv_media_as_in_person_meeting"]),
            ),
        )
        selection_metric = (
            "macro_f1, then accuracy, then fewer TV_MEDIA→IN_PERSON_MEETING errors"
        )
    selected = dict(candidates[best_index])
    selected["candidate_index"] = best_index
    best_pipeline = fitted[best_index]
    if best_pipeline is None:  # Defensive; valid_indices excludes failed candidates.
        raise RuntimeError(f"Selected classifier candidate failed for head {head!r}")
    return best_pipeline, {
        "enabled": True,
        "selection_split": "validation",
        "selection_metric": selection_metric,
        "cost_sensitive_activity": bool(cost_sensitive_activity and head == "activity"),
        "activity_cost_profile": activity_cost_profile,
        "selected": selected,
        "candidates": candidates,
    }


def _load_record_waveform(record: AudioRecord) -> np.ndarray:
    if record.start_seconds is not None:
        return load_wav_segment_16k_mono(
            record.path, record.start_seconds, record.end_seconds
        )
    return load_wav_16k_mono(record.path)


def _prefetched_batches(
    items: list[tuple[int, AudioRecord, str | None]],
    *,
    batch_size: int,
    prefetch_workers: int,
    prefetch_batches: int,
):
    """Bound HDD read-ahead while GPU inference consumes the previous batch."""

    if batch_size < 1 or prefetch_workers < 1 or prefetch_batches < 1:
        raise ValueError("Batch and prefetch values must be positive")

    next_offset = 0
    pending: deque[
        tuple[
            list[tuple[int, AudioRecord, str | None]],
            list[Future[np.ndarray]],
        ]
    ] = deque()

    with ThreadPoolExecutor(
        max_workers=prefetch_workers, thread_name_prefix="audio-prefetch"
    ) as executor:
        def submit_next() -> bool:
            nonlocal next_offset
            if next_offset >= len(items):
                return False
            batch = items[next_offset : next_offset + batch_size]
            next_offset += len(batch)
            pending.append(
                (batch, [executor.submit(_load_record_waveform, item[1]) for item in batch])
            )
            return True

        for _ in range(prefetch_batches):
            if not submit_next():
                break

        while pending:
            batch, futures = pending.popleft()
            waveforms = [future.result() for future in futures]
            submit_next()
            yield batch, waveforms


def benchmark_batch_sizes(
    records: list[AudioRecord],
    extractor: EmbeddingExtractor,
    *,
    batch_sizes: tuple[int, ...] = (1, 2, 4, 8, 16),
    sample_count: int = 32,
    prefetch_workers: int = 2,
) -> dict[str, object]:
    """Choose the fastest numerically equivalent YAMNet batch size."""

    extract_waveforms = getattr(extractor, "extract_waveforms", None)
    if extract_waveforms is None:
        return {
            "selected_batch_size": 1,
            "sample_count": 0,
            "reason": "extractor does not support batched waveforms",
            "results": [],
        }
    selected_count = min(len(records), max(1, sample_count))
    indices = np.linspace(0, len(records) - 1, num=selected_count, dtype=np.int64)
    # Manifests are path-sorted for HDD locality. Even spacing keeps the benchmark
    # representative across datasets, labels, and clip durations.
    selected_records = [records[int(index)] for index in indices]
    with ThreadPoolExecutor(max_workers=prefetch_workers) as executor:
        waveforms = list(executor.map(_load_record_waveform, selected_records))

    # Warm TensorFlow graphs and device kernels before measuring.
    extract_waveforms(waveforms[:1])
    reference: np.ndarray | None = None
    results: list[dict[str, object]] = []
    for requested_size in batch_sizes:
        size = max(1, int(requested_size))
        vectors: list[np.ndarray] = []
        started = perf_counter()
        try:
            for offset in range(0, len(waveforms), size):
                vectors.append(extract_waveforms(waveforms[offset : offset + size]))
            matrix = np.concatenate(vectors, axis=0)
            elapsed = perf_counter() - started
            if reference is None:
                reference = matrix
            max_difference = float(np.max(np.abs(matrix - reference)))
            equivalent = bool(np.allclose(matrix, reference, rtol=1e-5, atol=1e-5))
            results.append(
                {
                    "batch_size": size,
                    "seconds": elapsed,
                    "samples_per_second": len(waveforms) / max(elapsed, 1e-9),
                    "max_abs_difference": max_difference,
                    "equivalent_to_single": equivalent,
                }
            )
        except Exception as exc:  # OOM/unsupported sizes are benchmark results.
            results.append(
                {
                    "batch_size": size,
                    "error": f"{type(exc).__name__}: {exc}",
                    "equivalent_to_single": False,
                }
            )

    valid = [
        result
        for result in results
        if result.get("equivalent_to_single") and "samples_per_second" in result
    ]
    selected = max(valid, key=lambda item: item["samples_per_second"]) if valid else None
    return {
        "selected_batch_size": int(selected["batch_size"]) if selected else 1,
        "sample_count": len(waveforms),
        "prefetch_workers": prefetch_workers,
        "results": results,
    }


def _features(
    records: list[AudioRecord],
    extractor: EmbeddingExtractor,
    *,
    progress: Callable[[str], None] | None = None,
    stage: str = "features",
    batch_size: int = 1,
    prefetch_workers: int = 2,
    prefetch_batches: int = 2,
) -> tuple[np.ndarray, np.ndarray]:
    vectors: list[np.ndarray | None] = [None] * len(records)
    pending: list[tuple[int, AudioRecord, str | None]] = []
    cache = extractor if isinstance(extractor, CachedEmbeddingExtractor) else None
    base_extractor = cache.extractor if cache is not None else extractor
    stage_cache_hits = 0
    stage_cache_misses = 0

    if cache is None:
        pending.extend((index, record, None) for index, record in enumerate(records))
    else:
        cached_records = cache.lookup_many(
            [
                (record.path, record.start_seconds, record.end_seconds)
                for record in records
            ]
        )
        for index, (record, (key, cached)) in enumerate(
            zip(records, cached_records)
        ):
            if cached is None:
                pending.append((index, record, key))
                stage_cache_misses += 1
            else:
                vectors[index] = cached
                stage_cache_hits += 1
            scanned = index + 1
            if progress and (
                scanned == 1 or scanned % 500 == 0 or scanned == len(records)
            ):
                progress(
                    f"{stage} cache scan: {scanned}/{len(records)} "
                    f"(hits={stage_cache_hits}, misses={stage_cache_misses})"
                )

    completed = len(records) - len(pending)
    if progress and completed and cache is None:
        progress(f"{stage}: {completed}/{len(records)} (cache hits)")

    extract_waveforms = getattr(base_extractor, "extract_waveforms", None)
    if extract_waveforms is not None and pending:
        last_progress_completed = completed
        for batch, waveforms in _prefetched_batches(
            pending,
            batch_size=batch_size,
            prefetch_workers=prefetch_workers,
            prefetch_batches=prefetch_batches,
        ):
            batch_vectors = np.asarray(extract_waveforms(waveforms), dtype=np.float32)
            if batch_vectors.shape[0] != len(batch):
                raise RuntimeError(
                    f"Extractor returned {batch_vectors.shape[0]} vectors for "
                    f"a batch of {len(batch)}"
                )
            for (output_index, _record, key), vector in zip(batch, batch_vectors):
                values = cache.store(key, vector) if cache is not None else vector
                vectors[output_index] = values
            completed += len(batch)
            if progress and (
                completed - last_progress_completed >= 100
                or completed == len(records)
            ):
                progress(f"{stage}: {completed}/{len(records)}")
                last_progress_completed = completed
    else:
        extract_segment = getattr(base_extractor, "extract_segment", None)
        for output_index, record, key in pending:
            if record.start_seconds is not None and extract_segment is not None:
                vector = extract_segment(
                    record.path, record.start_seconds, record.end_seconds
                )
            else:
                vector = base_extractor.extract_file(record.path)
            values = cache.store(key, vector) if cache is not None else vector
            vectors[output_index] = values
            completed += 1
            if progress and (
                completed == 1
                or completed % 25 == 0
                or completed == len(records)
            ):
                progress(f"{stage}: {completed}/{len(records)}")

    if any(vector is None for vector in vectors):
        raise RuntimeError(f"{stage} did not produce every embedding")
    return np.stack(vectors), np.asarray([record.label for record in records])


def train_from_manifest(
    manifest_path: str | Path,
    output_dir: str | Path,
    *,
    extractor: EmbeddingExtractor | None = None,
    cache_path: str | Path | None = None,
    check_files: bool = True,
    verbose: bool = False,
    batch_size: int | str = 1,
    prefetch_workers: int = 2,
    prefetch_batches: int = 2,
    benchmark_sample_count: int = 32,
    feature_version: str = FEATURE_VERSION_MEAN_VAD_V1,
    tune_classifier: bool = False,
    classifier_c_values: tuple[float, ...] = DEFAULT_CLASSIFIER_C_VALUES,
    cost_sensitive_activity: bool = False,
    activity_cost_profile: str = "search",
    heads: tuple[str, ...] = ("activity", "sound_event"),
) -> dict[str, object]:
    records = list(iter_manifest(manifest_path, check_files=check_files))
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    base_extractor = extractor or YamnetEmbeddingExtractor(
        feature_version=feature_version
    )
    cached_extractor = (
        CachedEmbeddingExtractor(base_extractor, cache_path) if cache_path else None
    )
    feature_extractor = cached_extractor or base_extractor
    progress = (
        (lambda message: print(message, file=sys.stderr, flush=True))
        if verbose
        else None
    )
    benchmark: dict[str, object] | None = None
    if batch_size == "auto":
        benchmark = benchmark_batch_sizes(
            records,
            base_extractor,
            sample_count=benchmark_sample_count,
            prefetch_workers=prefetch_workers,
        )
        selected_batch_size = int(benchmark["selected_batch_size"])
    else:
        selected_batch_size = int(batch_size)
    if selected_batch_size < 1 or prefetch_workers < 1 or prefetch_batches < 1:
        raise ValueError("Batch and prefetch values must be positive")
    if not classifier_c_values or any(value <= 0 for value in classifier_c_values):
        raise ValueError("Classifier C values must be positive")
    allowed_heads = {"activity", "sound_event"}
    if not heads or set(heads) - allowed_heads:
        raise ValueError("heads must contain activity and/or sound_event")

    summary: dict[str, object] = {
        "yamnet": YAMNET_HANDLE,
        "feature_version": str(
            getattr(base_extractor, "feature_version", feature_version)
        ),
        "extraction": {
            "batch_size": selected_batch_size,
            "prefetch_workers": prefetch_workers,
            "prefetch_batches": prefetch_batches,
            "manifest_order": "path/start (HDD locality)",
            "benchmark": benchmark,
        },
        "heads": {},
    }

    try:
        for head in heads:
            head_records = [record for record in records if record.head == head]
            if not head_records:
                continue

            train_records = [
                record for record in head_records if record.split == "train"
            ]
            if len({record.label for record in train_records}) < 2:
                raise ValueError(f"Head {head!r} needs at least two training labels")

            x_train, y_train = _features(
                train_records,
                feature_extractor,
                progress=progress,
                stage=f"{head} train embeddings",
                batch_size=selected_batch_size,
                prefetch_workers=prefetch_workers,
                prefetch_batches=prefetch_batches,
            )

            evaluation_data: dict[str, tuple[np.ndarray, np.ndarray]] = {}
            for split in ("validation", "test"):
                evaluation_records = [
                    record for record in head_records if record.split == split
                ]
                if not evaluation_records:
                    continue
                evaluation_data[split] = _features(
                    evaluation_records,
                    feature_extractor,
                    progress=progress,
                    stage=f"{head} {split} embeddings",
                    batch_size=selected_batch_size,
                    prefetch_workers=prefetch_workers,
                    prefetch_batches=prefetch_batches,
                )

            if tune_classifier and "validation" in evaluation_data:
                x_validation, y_validation = evaluation_data["validation"]
                pipeline, classifier_selection = _select_classifier(
                    x_train,
                    y_train,
                    x_validation,
                    y_validation,
                    c_values=classifier_c_values,
                    progress=progress,
                    head=head,
                    cost_sensitive_activity=cost_sensitive_activity,
                    activity_cost_profile=activity_cost_profile,
                )
            else:
                pipeline = _classifier_pipeline()
                pipeline.fit(x_train, y_train)
                classifier_selection = {
                    "enabled": False,
                    "reason": (
                        "disabled"
                        if not tune_classifier
                        else "validation split unavailable"
                    ),
                    "selected": {
                        "C": 1.0,
                        "class_weight": "balanced",
                    },
                }

            active_feature_version = str(
                getattr(base_extractor, "feature_version", feature_version)
            )

            bundle = {
                "pipeline": pipeline,
                "head": head,
                "model_version": (
                    (
                        "yamnet-temporal-cost-logreg-v0.7.1-recall-buffer"
                        if activity_cost_profile == "recall-buffer"
                        else "yamnet-temporal-cost-logreg-v0.7.0"
                    )
                    if cost_sensitive_activity and head == "activity"
                    else (
                        "yamnet-vad-logreg-v0.5.0"
                        if active_feature_version == FEATURE_VERSION_MEAN_VAD_V1
                        else "yamnet-temporal-logreg-v0.6.0"
                    )
                ),
                "yamnet_handle": YAMNET_HANDLE,
                "feature_version": active_feature_version,
            }
            joblib.dump(bundle, output_path / f"{head}.joblib")

            head_summary: dict[str, object] = {
                "train_samples": len(train_records),
                "labels": sorted(set(y_train.tolist())),
                "classifier_selection": classifier_selection,
                "evaluations": {},
            }
            for split, (x_eval, y_eval) in evaluation_data.items():
                predicted = pipeline.predict(x_eval)
                evaluation_labels = sorted(
                    set(y_eval.tolist()) | set(predicted.tolist())
                )
                matrix = confusion_matrix(y_eval, predicted, labels=evaluation_labels)
                false_positive_rates: dict[str, float] = {}
                total = int(matrix.sum())
                for index, label in enumerate(evaluation_labels):
                    false_positives = int(
                        matrix[:, index].sum() - matrix[index, index]
                    )
                    true_negatives = int(
                        total
                        - matrix[index, :].sum()
                        - matrix[:, index].sum()
                        + matrix[index, index]
                    )
                    denominator = false_positives + true_negatives
                    false_positive_rates[label] = (
                        false_positives / denominator if denominator else 0.0
                    )
                head_summary["evaluations"][split] = {
                    "samples": len(y_eval),
                    "accuracy": float(accuracy_score(y_eval, predicted)),
                    "report": classification_report(
                        y_eval, predicted, output_dict=True, zero_division=0
                    ),
                    "confusion_matrix": {
                        "labels": evaluation_labels,
                        "values": matrix.tolist(),
                    },
                    "false_positive_rate": false_positive_rates,
                }
            summary["heads"][head] = head_summary
    finally:
        if cached_extractor is not None:
            summary["embedding_cache"] = cached_extractor.stats()
            cached_extractor.close()

    (output_path / "metrics.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    return summary


def predict_file(
    audio_path: str | Path,
    model_path: str | Path,
    *,
    extractor: EmbeddingExtractor | None = None,
) -> Prediction:
    bundle = joblib.load(model_path)
    feature_extractor = extractor or YamnetEmbeddingExtractor(
        bundle["yamnet_handle"],
        feature_version=bundle.get(
            "feature_version", FEATURE_VERSION_MEAN_VAD_V1
        ),
    )
    vector = feature_extractor.extract_file(audio_path).reshape(1, -1)
    pipeline = bundle["pipeline"]
    probabilities = pipeline.predict_proba(vector)[0]
    best_index = int(np.argmax(probabilities))
    label = str(pipeline.classes_[best_index])
    return Prediction(
        head=str(bundle["head"]),
        label=label,
        confidence=float(probabilities[best_index]),
        model_version=str(bundle["model_version"]),
    )
