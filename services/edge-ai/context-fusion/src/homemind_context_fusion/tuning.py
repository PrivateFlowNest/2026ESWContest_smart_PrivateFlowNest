"""Group-safe calibration of context thresholds without repeated YAMNet runs."""

from __future__ import annotations

from collections import Counter, defaultdict
import csv
from datetime import datetime, timedelta, timezone
import hashlib
from pathlib import Path
import statistics
import time
from typing import Callable, Iterable

from .audio_pipeline import DEFAULT_ACTIVITY_MODEL, DEFAULT_SOUND_MODEL, _import_audio_context
from .evaluation import _confusion_summary, build_evaluation_summary
from .models import ContextObservation, QUIET_REQUIRED_CONTEXTS
from .policy_config import ContextFusionPolicy


def _stable_rank(seed: int, label: str, group_id: str) -> bytes:
    return hashlib.sha256(f"{seed}:{label}:{group_id}".encode("utf-8")).digest()


def choose_group_disjoint_records(
    manifest: str | Path,
    *,
    split: str = "validation",
    holdout_split: str | None = None,
    calibration_per_label: int = 100,
    holdout_per_label: int = 100,
    minimum_seconds: float = 6.0,
    seed: int = 20260903,
) -> tuple[dict[str, list[dict[str, str]]], dict[str, object]]:
    """Select one segment per source group into disjoint calibration/holdout sets."""

    if min(calibration_per_label, holdout_per_label) < 1:
        raise ValueError("calibration and holdout counts must be positive")
    final_split = holdout_split or split
    grouped: dict[str, dict[str, dict[str, list[dict[str, str]]]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(list))
    )
    skipped_missing = 0
    skipped_short = 0
    with Path(manifest).open("r", encoding="utf-8-sig", newline="") as handle:
        for line_number, row in enumerate(csv.DictReader(handle), start=2):
            row_split = row.get("split")
            if row.get("head") != "activity" or row_split not in {split, final_split}:
                continue
            start = float(row.get("start_seconds") or 0.0)
            end = float(row.get("end_seconds") or 0.0)
            if end - start < minimum_seconds:
                skipped_short += 1
                continue
            if not Path(row["path"]).is_file():
                skipped_missing += 1
                continue
            group_id = (row.get("group_id") or "").strip()
            if not group_id:
                raise ValueError(f"Manifest row {line_number} has no group_id")
            grouped[str(row_split)][row["label"]][group_id].append(row)

    partitions = {"calibration": [], "holdout": []}
    available_groups: dict[str, dict[str, int]] = {}
    selected_counts: dict[str, dict[str, int]] = {}
    calibration_labels = set(grouped[split])
    holdout_labels = set(grouped[final_split])
    if calibration_labels != holdout_labels:
        raise ValueError(
            "calibration and holdout splits must contain the same activity labels"
        )
    for label in sorted(calibration_labels):
        calibration_source = grouped[split][label]
        holdout_source = grouped[final_split][label]
        available_groups[label] = {
            "calibration_split": len(calibration_source),
            "holdout_split": len(holdout_source),
        }
        if split == final_split:
            required = calibration_per_label + holdout_per_label
            if len(calibration_source) < required:
                raise ValueError(
                    f"{label} has {len(calibration_source)} eligible groups; "
                    f"{required} required"
                )
            ordered = sorted(
                calibration_source,
                key=lambda group_id: _stable_rank(seed, label, group_id),
            )
            calibration_groups = ordered[:calibration_per_label]
            holdout_groups = ordered[
                calibration_per_label : calibration_per_label + holdout_per_label
            ]
        else:
            if len(calibration_source) < calibration_per_label:
                raise ValueError(
                    f"{label} has {len(calibration_source)} calibration groups; "
                    f"{calibration_per_label} required"
                )
            if len(holdout_source) < holdout_per_label:
                raise ValueError(
                    f"{label} has {len(holdout_source)} holdout groups; "
                    f"{holdout_per_label} required"
                )
            calibration_groups = sorted(
                calibration_source,
                key=lambda group_id: _stable_rank(seed, label, group_id),
            )[:calibration_per_label]
            holdout_groups = sorted(
                holdout_source,
                key=lambda group_id: _stable_rank(seed + 1, label, group_id),
            )[:holdout_per_label]
        selected_counts[label] = {
            "calibration": len(calibration_groups),
            "holdout": len(holdout_groups),
        }
        for partition, source_groups, group_ids in (
            ("calibration", calibration_source, calibration_groups),
            ("holdout", holdout_source, holdout_groups),
        ):
            for group_id in group_ids:
                rows = sorted(
                    source_groups[group_id],
                    key=lambda row: (
                        row["path"].casefold(),
                        float(row.get("start_seconds") or 0.0),
                    ),
                )
                partitions[partition].append(rows[0])

    calibration_groups = {
        row["group_id"] for row in partitions["calibration"]
    }
    holdout_groups = {row["group_id"] for row in partitions["holdout"]}
    overlap = calibration_groups & holdout_groups
    if overlap:
        raise RuntimeError("calibration and holdout source groups overlap")
    return partitions, {
        "calibration_split": split,
        "holdout_split": final_split,
        "seed": seed,
        "minimum_seconds": minimum_seconds,
        "available_group_counts": available_groups,
        "selected_counts": selected_counts,
        "group_overlap_count": 0,
        "skipped_missing": skipped_missing,
        "skipped_short": skipped_short,
        "selection_rule": "stable hash; one segment per source group; split boundary preserved",
    }


def collect_prediction_cases(
    rows: Iterable[dict[str, str]],
    *,
    predictor,
    load_segment,
    iter_windows,
    vad_features,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
    progress: Callable[[str], None] | None = None,
) -> tuple[list[dict[str, object]], list[float]]:
    """Run YAMNet once and retain privacy-safe per-window predictions in memory."""

    selected = list(rows)
    cases: list[dict[str, object]] = []
    timings: list[float] = []
    for index, row in enumerate(selected, start=1):
        start = float(row.get("start_seconds") or 0.0)
        end = float(row["end_seconds"])
        waveform = load_segment(Path(row["path"]), start, end)
        windows: list[dict[str, object]] = []
        for window_index, window in enumerate(
            iter_windows(
                waveform,
                window_seconds=window_seconds,
                hop_seconds=hop_seconds,
            )
        ):
            started = time.perf_counter_ns()
            activity, _sound = predictor.predict_waveform(window)
            speech_ratio = float(vad_features(window)[0])
            timings.append((time.perf_counter_ns() - started) / 1_000_000)
            windows.append(
                {
                    "offset_seconds": window_index * hop_seconds,
                    "label": activity.label,
                    "confidence": float(activity.confidence),
                    "class_probabilities": getattr(
                        activity, "class_probabilities", None
                    ),
                    "model_version": str(activity.model_version),
                    "speech_ratio": speech_ratio,
                }
            )
        if windows:
            cases.append(
                {
                    "expected": row["label"],
                    "duration_seconds": end - start,
                    "windows": windows,
                }
            )
        if progress and (index == 1 or index % 25 == 0 or index == len(selected)):
            progress(f"audio evidence: {index}/{len(selected)}")
    return cases, timings


def evaluate_prediction_cases(
    cases: Iterable[dict[str, object]],
    policy: ContextFusionPolicy,
    *,
    window_seconds: float = 4.0,
    hop_seconds: float = 1.0,
    release_probe_windows: int | None = None,
    model_version: str = "yamnet-temporal-logreg-v0.6.0+context-fusion-tuned-v1",
) -> dict[str, object]:
    """Replay cached predictions through one policy, including a release proxy.

    The no-speech tail lasts ``release_probe_windows`` windows; by default 6, or ``release_updates + 4`` for
    hold policies that need longer than that to release (the v5/v6 reports used 6).
    """

    if release_probe_windows is None:
        release_probe_windows = max(6, policy.release_updates + 4)
    if release_probe_windows < 1:
        raise ValueError("release_probe_windows must be positive")
    outcomes: list[dict[str, object]] = []
    window_outcomes: list[dict[str, object]] = []
    segment_outcomes: list[dict[str, object]] = []
    base_time = datetime(2026, 9, 3, tzinfo=timezone.utc)
    for case_index, case in enumerate(cases, start=1):
        engine = policy.engine()
        frames: list[tuple[float, dict[str, object]]] = []
        raw_labels: list[str] = []
        for raw in case["windows"]:
            offset = float(raw["offset_seconds"])
            raw_label = str(raw["label"])
            raw_labels.append(raw_label)
            window_outcomes.append(
                {"expected": case["expected"], "predicted": raw_label}
            )
            observation = ContextObservation(
                audio_context=raw_label,
                audio_confidence=float(raw["confidence"]),
                audio_class_probabilities=raw.get("class_probabilities"),
                audio_model_version=str(raw["model_version"]),
                speech_ratio=float(raw["speech_ratio"]),
                duration_seconds=window_seconds + offset,
                trace_id=f"tune-{case_index:04d}",
                timestamp=base_time + timedelta(seconds=window_seconds + offset),
            )
            frames.append((offset, engine.update(observation)))

        segment_outcomes.append(
            {
                "expected": case["expected"],
                "predicted": Counter(raw_labels).most_common(1)[0][0],
            }
        )
        activated = [
            (offset, event)
            for offset, event in frames
            if event["transition"] == "ACTIVATED" and event["quiet_required"] is True
        ]
        if activated:
            first_offset, first_event = activated[0]
            predicted = str(first_event["context_type"])
            activation_delay = first_offset + window_seconds
            quiet_required = True
        else:
            stable = [
                str(event["context_type"])
                for _offset, event in frames
                if event["context_type"] != "UNKNOWN"
            ]
            predicted = Counter(stable).most_common(1)[0][0] if stable else "UNKNOWN"
            activation_delay = None
            quiet_required = False

        release_delay: float | None = None
        if activated and not bool(frames[-1][1]["quiet_required"]):
            release_delay = 0.0
        elif activated:
            duration = float(case["duration_seconds"])
            for probe_index in range(release_probe_windows):
                decision_seconds = duration + window_seconds + probe_index * hop_seconds
                event = engine.update(
                    ContextObservation(
                        audio_context="UNKNOWN",
                        audio_confidence=0.0,
                        speech_ratio=0.0,
                        duration_seconds=decision_seconds,
                        trace_id=f"tune-{case_index:04d}",
                        timestamp=base_time + timedelta(seconds=decision_seconds),
                    )
                )
                if event["transition"] == "DEACTIVATED":
                    release_delay = decision_seconds - duration
                    break

        outcomes.append(
            {
                "expected": case["expected"],
                "predicted": predicted,
                "quiet_required": quiet_required,
                "activation_delay_seconds": activation_delay,
                "release_delay_seconds": release_delay,
            }
        )

    report = build_evaluation_summary(
        outcomes,
        latency_ms=[],
        model_version=model_version,
    )
    report["policy"] = policy.as_dict()
    report["audio_classifier_only"] = {
        "window": _confusion_summary(window_outcomes),
        "segment_majority": _confusion_summary(segment_outcomes),
    }
    return report


def _gate_result(
    report: dict[str, object],
    *,
    maximum_false_positive_rate: float,
    minimum_recall: float,
    maximum_activation_p95_seconds: float,
    maximum_release_p95_seconds: float,
) -> dict[str, object]:
    quiet = report["quiet_required"]
    activation = report["activation_delay_seconds"]
    release = report["release_delay_seconds"]
    checks = {
        "false_positive_rate_below_maximum": (
            float(quiet["false_positive_rate"]) < maximum_false_positive_rate
        ),
        "recall_at_least_minimum": float(quiet["recall"]) >= minimum_recall,
        "activation_p95_within_maximum": (
            activation["p95"] is not None
            and float(activation["p95"]) <= maximum_activation_p95_seconds
        ),
        "release_p95_within_maximum": (
            release["p95"] is not None
            and float(release["p95"]) <= maximum_release_p95_seconds
        ),
    }
    return {"passed": all(checks.values()), "checks": checks}


def tune_policy(
    calibration_cases: Iterable[dict[str, object]],
    holdout_cases: Iterable[dict[str, object]],
    *,
    quiet_thresholds: Iterable[float],
    meeting_tv_margins: Iterable[float],
    activation_updates: Iterable[int],
    release_updates: Iterable[int],
    maximum_false_positive_rate: float = 0.05,
    minimum_recall: float = 0.90,
    maximum_activation_p95_seconds: float = 8.0,
    maximum_release_p95_seconds: float = 8.0,
) -> dict[str, object]:
    calibration = list(calibration_cases)
    holdout = list(holdout_cases)
    candidates: list[dict[str, object]] = []
    reports: list[dict[str, object]] = []
    for threshold in quiet_thresholds:
        for meeting_tv_margin in meeting_tv_margins:
            for enter in activation_updates:
                for release in release_updates:
                    policy = ContextFusionPolicy(
                        quiet_audio_confidence=float(threshold),
                        meeting_tv_margin=float(meeting_tv_margin),
                        activation_updates=int(enter),
                        release_updates=int(release),
                    )
                    report = evaluate_prediction_cases(calibration, policy)
                    gate = _gate_result(
                        report,
                        maximum_false_positive_rate=maximum_false_positive_rate,
                        minimum_recall=minimum_recall,
                        maximum_activation_p95_seconds=maximum_activation_p95_seconds,
                        maximum_release_p95_seconds=maximum_release_p95_seconds,
                    )
                    candidate = {
                        "policy": policy.as_dict(),
                        "passed": gate["passed"],
                        "exact_context_accuracy": report["exact_context_accuracy"],
                        "quiet_recall": report["quiet_required"]["recall"],
                        "quiet_false_positive_rate": report["quiet_required"][
                            "false_positive_rate"
                        ],
                        "activation_p95_seconds": report["activation_delay_seconds"]["p95"],
                        "release_p95_seconds": report["release_delay_seconds"]["p95"],
                    }
                    candidates.append(candidate)
                    reports.append(report)

    if not candidates:
        raise ValueError("at least one policy candidate is required")
    passing = [index for index, value in enumerate(candidates) if value["passed"]]
    if passing:
        selected_index = max(
            passing,
            key=lambda index: (
                float(candidates[index]["quiet_recall"]),
                -float(candidates[index]["quiet_false_positive_rate"]),
                float(candidates[index]["exact_context_accuracy"]),
                -float(candidates[index]["activation_p95_seconds"]),
                -float(candidates[index]["release_p95_seconds"]),
            ),
        )
        selection_reason = (
            "highest quiet recall, then lowest false-positive rate and highest exact "
            "accuracy among policies passing every gate"
        )
    else:
        def violation(index: int) -> tuple[float, float]:
            value = candidates[index]
            amount = (
                max(
                    0.0,
                    float(value["quiet_false_positive_rate"])
                    - maximum_false_positive_rate,
                )
                + max(0.0, minimum_recall - float(value["quiet_recall"]))
                + max(
                    0.0,
                    float(value["activation_p95_seconds"] or 999.0)
                    - maximum_activation_p95_seconds,
                )
                / maximum_activation_p95_seconds
                + max(
                    0.0,
                    float(value["release_p95_seconds"] or 999.0)
                    - maximum_release_p95_seconds,
                )
                / maximum_release_p95_seconds
            )
            return (-amount, float(value["exact_context_accuracy"]))

        selected_index = max(range(len(candidates)), key=violation)
        selection_reason = "no policy passed every gate; smallest normalized violation"

    selected_policy = ContextFusionPolicy.from_mapping(
        candidates[selected_index]["policy"]
    )
    holdout_report = evaluate_prediction_cases(holdout, selected_policy)
    holdout_gate = _gate_result(
        holdout_report,
        maximum_false_positive_rate=maximum_false_positive_rate,
        minimum_recall=minimum_recall,
        maximum_activation_p95_seconds=maximum_activation_p95_seconds,
        maximum_release_p95_seconds=maximum_release_p95_seconds,
    )
    return {
        "schema_version": "1.0",
        "gates": {
            "quiet_false_positive_rate": f"<{maximum_false_positive_rate}",
            "quiet_recall": f">={minimum_recall}",
            "activation_p95_seconds": f"<={maximum_activation_p95_seconds}",
            "release_p95_seconds": f"<={maximum_release_p95_seconds}",
        },
        "selection_reason": selection_reason,
        "candidate_count": len(candidates),
        "selected_candidate": candidates[selected_index],
        "calibration": {
            **reports[selected_index],
            "gate": _gate_result(
                reports[selected_index],
                maximum_false_positive_rate=maximum_false_positive_rate,
                minimum_recall=minimum_recall,
                maximum_activation_p95_seconds=maximum_activation_p95_seconds,
                maximum_release_p95_seconds=maximum_release_p95_seconds,
            ),
        },
        "holdout": {**holdout_report, "gate": holdout_gate},
        "candidates": candidates,
    }


def run_audio_policy_tuning(
    manifest: str | Path,
    *,
    activity_model: str | Path = DEFAULT_ACTIVITY_MODEL,
    sound_model: str | Path = DEFAULT_SOUND_MODEL,
    calibration_per_label: int = 100,
    holdout_per_label: int = 100,
    seed: int = 20260903,
    calibration_split: str = "validation",
    holdout_split: str | None = None,
    quiet_thresholds: Iterable[float] = (0.0, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90),
    meeting_tv_margins: Iterable[float] = (0.0, 0.025, 0.05, 0.075, 0.10, 0.15, 0.20, 0.25),
    activation_updates: Iterable[int] = (2, 3, 4, 5),
    release_updates: Iterable[int] = (2, 3, 4),
    maximum_release_p95_seconds: float = 8.0,
    progress: Callable[[str], None] | None = None,
) -> dict[str, object]:
    partitions, selection = choose_group_disjoint_records(
        manifest,
        split=calibration_split,
        holdout_split=holdout_split,
        calibration_per_label=calibration_per_label,
        holdout_per_label=holdout_per_label,
        seed=seed,
    )
    (_load_wav, load_segment, vad_features, predictor_type, iter_windows) = (
        _import_audio_context()
    )
    predictor = predictor_type(Path(activity_model), Path(sound_model))
    cases: dict[str, list[dict[str, object]]] = {}
    timings: list[float] = []
    for partition in ("calibration", "holdout"):
        if progress:
            progress(f"collecting {partition} audio evidence")
        cases[partition], partition_timings = collect_prediction_cases(
            partitions[partition],
            predictor=predictor,
            load_segment=load_segment,
            iter_windows=iter_windows,
            vad_features=vad_features,
            progress=progress,
        )
        timings.extend(partition_timings)

    result = tune_policy(
        cases["calibration"],
        cases["holdout"],
        quiet_thresholds=quiet_thresholds,
        meeting_tv_margins=meeting_tv_margins,
        activation_updates=activation_updates,
        release_updates=release_updates,
        maximum_release_p95_seconds=maximum_release_p95_seconds,
    )
    sorted_timings = sorted(timings)
    result["selection"] = selection
    result["inference_collection"] = {
        "windows": len(timings),
        "mean_ms": round(statistics.fmean(timings), 3) if timings else None,
        "p95_ms": (
            round(sorted_timings[min(len(sorted_timings) - 1, int(len(sorted_timings) * 0.95))], 3)
            if timings
            else None
        ),
    }
    result["privacy"] = {
        "raw_audio_in_report": False,
        "file_paths_in_report": False,
        "group_ids_in_report": False,
        "transcripts_in_report": False,
    }
    return result
