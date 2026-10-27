"""Small, repeatable real-audio evaluation for the fused PC vertical slice."""

from __future__ import annotations

from collections import Counter, defaultdict
import csv
from datetime import datetime, timezone
from pathlib import Path
import random
import statistics
from typing import Iterable

from .audio_pipeline import (
    DEFAULT_ACTIVITY_MODEL,
    DEFAULT_SOUND_MODEL,
    AudioFusionFrame,
    AudioFusionRunner,
    _import_audio_context,
)
from .engine import ContextFusionEngine
from .models import QUIET_REQUIRED_CONTEXTS


def _choose_records(
    manifest: Path,
    *,
    split: str,
    per_label: int,
    minimum_seconds: float,
    seed: int,
) -> tuple[list[dict[str, str]], dict[str, int]]:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    skipped_missing = 0
    with manifest.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("head") != "activity" or row.get("split") != split:
                continue
            start = float(row.get("start_seconds") or 0.0)
            end = float(row.get("end_seconds") or 0.0)
            if end - start < minimum_seconds:
                continue
            if not Path(row["path"]).is_file():
                skipped_missing += 1
                continue
            grouped[row["label"]].append(row)

    rng = random.Random(seed)
    selected: list[dict[str, str]] = []
    available: dict[str, int] = {}
    for label in sorted(grouped):
        rows = grouped[label]
        available[label] = len(rows)
        rng.shuffle(rows)
        selected.extend(rows[:per_label])
    return selected, {"skipped_missing": skipped_missing, **available}


def _record_prediction(
    frames: list[AudioFusionFrame], *, window_seconds: float
) -> tuple[str, bool, float | None]:
    activated = [
        item
        for item in frames
        if item.context_event["transition"] == "ACTIVATED"
        and item.context_event["quiet_required"] is True
    ]
    if activated:
        first = activated[0]
        return (
            str(first.context_event["context_type"]),
            True,
            first.offset_seconds + window_seconds,
        )
    stable = [
        str(item.context_event["context_type"])
        for item in frames
        if item.context_event["context_type"] != "UNKNOWN"
    ]
    predicted = Counter(stable).most_common(1)[0][0] if stable else "UNKNOWN"
    return predicted, False, None


def _confusion_summary(
    rows: Iterable[dict[str, object]],
) -> dict[str, object]:
    values = list(rows)
    labels = sorted(
        {str(row["expected"]) for row in values}
        | {str(row["predicted"]) for row in values}
    )
    matrix = [
        [
            sum(
                row["expected"] == expected and row["predicted"] == predicted
                for row in values
            )
            for predicted in labels
        ]
        for expected in labels
    ]
    return {
        "samples": len(values),
        "accuracy": round(
            sum(row["expected"] == row["predicted"] for row in values)
            / max(len(values), 1),
            4,
        ),
        "confusion_matrix": {"labels": labels, "values": matrix},
    }


def build_evaluation_summary(
    outcomes: Iterable[dict[str, object]],
    *,
    latency_ms: Iterable[float],
    model_version: str,
) -> dict[str, object]:
    rows = list(outcomes)
    timings = sorted(float(value) for value in latency_ms)
    labels = sorted(
        {str(row["expected"]) for row in rows}
        | {str(row["predicted"]) for row in rows}
    )
    matrix = [
        [
            sum(
                row["expected"] == expected and row["predicted"] == predicted
                for row in rows
            )
            for predicted in labels
        ]
        for expected in labels
    ]
    expected_quiet = [
        row for row in rows if str(row["expected"]) in QUIET_REQUIRED_CONTEXTS
    ]
    expected_non_quiet = [
        row for row in rows if str(row["expected"]) not in QUIET_REQUIRED_CONTEXTS
    ]
    activation_delays = [
        float(row["activation_delay_seconds"])
        for row in expected_quiet
        if row.get("activation_delay_seconds") is not None
    ]
    release_delays = [
        float(row["release_delay_seconds"])
        for row in expected_quiet
        if row.get("release_delay_seconds") is not None
    ]
    percentile = lambda samples, fraction: samples[
        min(len(samples) - 1, int(len(samples) * fraction))
    ]
    return {
        "schema_version": "1.0",
        "model_version": model_version,
        "samples": len(rows),
        "exact_context_accuracy": round(
            sum(row["expected"] == row["predicted"] for row in rows) / max(len(rows), 1),
            4,
        ),
        "quiet_required": {
            "positive_samples": len(expected_quiet),
            "negative_samples": len(expected_non_quiet),
            "recall": round(
                sum(bool(row["quiet_required"]) for row in expected_quiet)
                / max(len(expected_quiet), 1),
                4,
            ),
            "false_positive_rate": round(
                sum(bool(row["quiet_required"]) for row in expected_non_quiet)
                / max(len(expected_non_quiet), 1),
                4,
            ),
        },
        "activation_delay_seconds": {
            "count": len(activation_delays),
            "mean": (
                round(statistics.fmean(activation_delays), 3)
                if activation_delays
                else None
            ),
            "p95": (
                round(percentile(sorted(activation_delays), 0.95), 3)
                if activation_delays
                else None
            ),
        },
        "release_delay_seconds": {
            "count": len(release_delays),
            "mean": (
                round(statistics.fmean(release_delays), 3)
                if release_delays
                else None
            ),
            "p95": (
                round(percentile(sorted(release_delays), 0.95), 3)
                if release_delays
                else None
            ),
            "method": "synthetic no-speech tail proxy; confirm with real transition audio",
        },
        "confusion_matrix": {"labels": labels, "values": matrix},
        "inference_latency_ms": {
            "windows": len(timings),
            "mean": round(statistics.fmean(timings), 3) if timings else None,
            "p95": round(percentile(timings, 0.95), 3) if timings else None,
        },
        "coverage": {
            "learned_activity_labels": [
                "IN_PERSON_MEETING",
                "SELF_TALK",
                "TV_MEDIA",
                "UNKNOWN",
                "ZOOM_SPEAKER",
            ],
            "rule_or_auxiliary_labels": ["QUIET", "ZOOM_HEADPHONES"],
        },
        "privacy": {
            "raw_audio_in_report": False,
            "file_paths_in_report": False,
            "transcripts_in_report": False,
        },
    }


def evaluate_manifest(
    manifest: str | Path,
    *,
    activity_model: str | Path = DEFAULT_ACTIVITY_MODEL,
    sound_model: str | Path = DEFAULT_SOUND_MODEL,
    split: str = "validation",
    per_label: int = 5,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
    seed: int = 42,
) -> dict[str, object]:
    if per_label < 1:
        raise ValueError("per_label must be positive")
    (
        _load_wav,
        load_segment,
        vad_features,
        predictor_type,
        iter_windows,
    ) = _import_audio_context()
    selected, available = _choose_records(
        Path(manifest),
        split=split,
        per_label=per_label,
        minimum_seconds=window_seconds + 2 * hop_seconds,
        seed=seed,
    )
    predictor = predictor_type(Path(activity_model), Path(sound_model))
    outcomes: list[dict[str, object]] = []
    window_outcomes: list[dict[str, object]] = []
    segment_audio_outcomes: list[dict[str, object]] = []
    timings: list[float] = []
    for index, row in enumerate(selected, 1):
        waveform = load_segment(
            Path(row["path"]),
            float(row.get("start_seconds") or 0.0),
            float(row["end_seconds"]),
        )
        runner = AudioFusionRunner(
            predictor,
            lambda window: float(vad_features(window)[0]),
            engine=ContextFusionEngine(),
        )
        frames = list(
            runner.process_windows(
                iter_windows(
                    waveform,
                    window_seconds=window_seconds,
                    hop_seconds=hop_seconds,
                ),
                trace_id=f"eval-{index:04d}",
                base_time=datetime(2026, 9, 3, tzinfo=timezone.utc),
                hop_seconds=hop_seconds,
                window_seconds=window_seconds,
            )
        )
        if not frames:
            continue
        raw_labels = [str(frame.activity_prediction["label"]) for frame in frames]
        for raw_label in raw_labels:
            window_outcomes.append(
                {"expected": row["label"], "predicted": raw_label}
            )
        segment_audio_outcomes.append(
            {
                "expected": row["label"],
                "predicted": Counter(raw_labels).most_common(1)[0][0],
            }
        )
        predicted, quiet_required, delay = _record_prediction(
            frames, window_seconds=window_seconds
        )
        outcomes.append(
            {
                "expected": row["label"],
                "predicted": predicted,
                "quiet_required": quiet_required,
                "activation_delay_seconds": delay,
            }
        )
        timings.extend(frame.latency_ms for frame in frames)

    report = build_evaluation_summary(
        outcomes,
        latency_ms=timings[1:] if len(timings) > 1 else timings,
        model_version="yamnet-temporal-logreg-v0.6.0+context-fusion-rules-v1",
    )
    report["selection"] = {
        "split": split,
        "per_label": per_label,
        "seed": seed,
        "available_after_duration_filter": available,
    }
    report["audio_classifier_only"] = {
        "window": _confusion_summary(window_outcomes),
        "segment_majority": _confusion_summary(segment_audio_outcomes),
        "note": "No fusion rules, hysteresis, vision, or device state applied.",
    }
    return report
