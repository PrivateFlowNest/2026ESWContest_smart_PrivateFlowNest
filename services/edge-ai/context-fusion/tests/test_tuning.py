from __future__ import annotations

import csv

from homemind_context_fusion.policy_config import ContextFusionPolicy
from homemind_context_fusion.tuning import (
    choose_group_disjoint_records,
    evaluate_prediction_cases,
    tune_policy,
)


def _case(expected, label, confidence):
    return {
        "expected": expected,
        "duration_seconds": 8.0,
        "windows": [
            {
                "offset_seconds": float(index),
                "label": label,
                "confidence": confidence,
                "model_version": "fixture",
                "speech_ratio": 0.5,
            }
            for index in range(5)
        ],
    }


def test_group_disjoint_selection_uses_one_record_per_group(tmp_path):
    manifest = tmp_path / "manifest.csv"
    rows = []
    for label in ("IN_PERSON_MEETING", "TV_MEDIA"):
        for group in range(4):
            wav = tmp_path / f"{label}-{group}.wav"
            wav.touch()
            for segment in range(2):
                rows.append(
                    {
                        "path": str(wav),
                        "head": "activity",
                        "label": label,
                        "split": "validation",
                        "start_seconds": str(segment * 8),
                        "end_seconds": str(segment * 8 + 8),
                        "group_id": f"{label}-{group}",
                    }
                )
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)

    partitions, report = choose_group_disjoint_records(
        manifest,
        calibration_per_label=2,
        holdout_per_label=2,
    )

    calibration = {row["group_id"] for row in partitions["calibration"]}
    holdout = {row["group_id"] for row in partitions["holdout"]}
    assert len(partitions["calibration"]) == 4
    assert len(partitions["holdout"]) == 4
    assert not calibration.intersection(holdout)
    assert report["group_overlap_count"] == 0


def test_policy_tuning_meets_false_positive_recall_and_delay_gates():
    cases = [
        _case("IN_PERSON_MEETING", "IN_PERSON_MEETING", 0.90),
        _case("TV_MEDIA", "IN_PERSON_MEETING", 0.55),
    ]

    result = tune_policy(
        cases,
        cases,
        quiet_thresholds=(0.0, 0.70),
        meeting_tv_margins=(0.0,),
        activation_updates=(2, 3),
        release_updates=(2,),
    )

    assert result["selected_candidate"]["policy"]["quiet_audio_confidence"] == 0.70
    assert result["calibration"]["gate"]["passed"] is True
    assert result["holdout"]["gate"]["passed"] is True
    assert result["holdout"]["quiet_required"]["recall"] == 1.0
    assert result["holdout"]["quiet_required"]["false_positive_rate"] == 0.0


def test_release_delay_uses_synthetic_no_speech_tail():
    report = evaluate_prediction_cases(
        [_case("IN_PERSON_MEETING", "IN_PERSON_MEETING", 0.9)],
        ContextFusionPolicy(activation_updates=2, release_updates=2),
    )

    assert report["activation_delay_seconds"]["p95"] == 5.0
    assert report["release_delay_seconds"]["p95"] == 5.0
    assert "synthetic" in report["release_delay_seconds"]["method"]


def test_release_tail_is_long_enough_for_hold_policies():
    report = evaluate_prediction_cases(
        [_case("IN_PERSON_MEETING", "IN_PERSON_MEETING", 0.9)],
        ContextFusionPolicy(activation_updates=3, release_updates=15),
    )

    assert report["release_delay_seconds"]["count"] == 1
    assert report["release_delay_seconds"]["p95"] == 18.0


def test_group_selection_can_use_pre_reserved_test_split(tmp_path):
    manifest = tmp_path / "manifest.csv"
    rows = []
    for label in ("IN_PERSON_MEETING", "TV_MEDIA"):
        for split_name in ("validation", "test"):
            for index in range(2):
                wav = tmp_path / f"{label}-{split_name}-{index}.wav"
                wav.write_bytes(b"RIFF")
                rows.append(
                    {
                        "path": str(wav),
                        "head": "activity",
                        "label": label,
                        "split": split_name,
                        "start_seconds": "0",
                        "end_seconds": "8",
                        "group_id": f"{label}-{split_name}-{index}",
                    }
                )
    with manifest.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)

    partitions, report = choose_group_disjoint_records(
        manifest,
        split="validation",
        holdout_split="test",
        calibration_per_label=1,
        holdout_per_label=1,
    )

    assert {row["split"] for row in partitions["calibration"]} == {"validation"}
    assert {row["split"] for row in partitions["holdout"]} == {"test"}
    assert report["group_overlap_count"] == 0
