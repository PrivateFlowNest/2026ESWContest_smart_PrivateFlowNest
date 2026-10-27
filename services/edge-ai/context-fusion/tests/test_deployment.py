from __future__ import annotations

import json

import pytest

from homemind_context_fusion.deployment import (
    build_release_candidate,
    sha256_file,
    verify_release_candidate,
)


def _json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def test_release_candidate_hashes_files_and_blocks_activation(tmp_path):
    activity = tmp_path / "activity.joblib"
    activity.write_bytes(b"activity")
    sound = tmp_path / "sound.joblib"
    sound.write_bytes(b"sound")
    metrics = _json(tmp_path / "metrics.json", {"accuracy": 0.85})
    config = _json(
        tmp_path / "policy.json",
        {
            "acceptance": {
                "calibration_passed": False,
                "holdout_passed": False,
            }
        },
    )
    report = _json(
        tmp_path / "report.json",
        {
            "calibration": {"gate": {"passed": False}},
            "holdout": {"gate": {"passed": False}},
        },
    )

    output = tmp_path / "release"
    manifest = build_release_candidate(
        output,
        activity_model=activity,
        sound_model=sound,
        policy_config=config,
        training_metrics=metrics,
        tuning_report=report,
        package_version="audio-context-test-v1",
        rollback_model_version="test-rollback-v1",
    )

    assert manifest["activation"]["allowed"] is False
    assert "holdout_gate_failed" in manifest["activation"]["blockers"]
    record = next(item for item in manifest["files"] if item["role"] == "activity_model")
    assert record["sha256"] == sha256_file(output / record["path"])
    assert not str(record["path"]).startswith(str(tmp_path))
    assert manifest["privacy"]["raw_audio_included"] is False
    assert manifest["package_version"] == "audio-context-test-v1"
    assert manifest["rollback"]["model_version"] == "test-rollback-v1"
    assert verify_release_candidate(output)["valid"] is True

    (output / record["path"]).write_bytes(b"tampered")
    assert verify_release_candidate(output)["valid"] is False


def test_release_candidate_rejects_acceptance_mismatch(tmp_path):
    model = tmp_path / "model"
    model.write_bytes(b"model")
    metrics = _json(tmp_path / "metrics.json", {})
    config = _json(
        tmp_path / "policy.json",
        {"acceptance": {"calibration_passed": True, "holdout_passed": True}},
    )
    report = _json(
        tmp_path / "report.json",
        {
            "calibration": {"gate": {"passed": False}},
            "holdout": {"gate": {"passed": False}},
        },
    )

    with pytest.raises(ValueError, match="does not match"):
        build_release_candidate(
            tmp_path / "release",
            activity_model=model,
            sound_model=model,
            policy_config=config,
            training_metrics=metrics,
            tuning_report=report,
        )
