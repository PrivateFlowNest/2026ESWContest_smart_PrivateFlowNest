"""Build a self-describing, hash-verified Pi 5 release candidate bundle."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
from typing import Iterable


def sha256_file(path: str | Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_object(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def _copy_record(
    source: Path,
    destination: Path,
    role: str,
    *,
    root: Path,
) -> dict[str, object]:
    if not source.is_file():
        raise FileNotFoundError(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return {
        "role": role,
        "path": destination.relative_to(root).as_posix(),
        "bytes": destination.stat().st_size,
        "sha256": sha256_file(destination),
    }


def build_release_candidate(
    output_dir: str | Path,
    *,
    activity_model: str | Path,
    sound_model: str | Path,
    policy_config: str | Path,
    training_metrics: str | Path,
    tuning_report: str | Path,
    evaluation_report: str | Path | None = None,
    runtime_wheels: Iterable[str | Path] = (),
    rollback_activity_model: str | Path | None = None,
    rollback_sound_model: str | Path | None = None,
    rollback_metrics: str | Path | None = None,
    package_version: str | None = None,
    rollback_model_version: str = "public-balanced-v5-window4s",
) -> dict[str, object]:
    """Create a new bundle; an existing destination is never overwritten."""

    target = Path(output_dir)
    if target.exists():
        raise FileExistsError(f"release destination already exists: {target}")

    config_path = Path(policy_config)
    report_path = Path(tuning_report)
    config = _read_object(config_path)
    report = _read_object(report_path)
    acceptance = config.get("acceptance")
    if not isinstance(acceptance, dict):
        raise ValueError("policy config must contain acceptance results")
    calibration_passed = acceptance.get("calibration_passed") is True
    holdout_passed = acceptance.get("holdout_passed") is True
    report_calibration = report.get("calibration", {})
    report_holdout = report.get("holdout", {})
    if not isinstance(report_calibration, dict) or not isinstance(report_holdout, dict):
        raise ValueError("tuning report must contain calibration and holdout results")
    report_calibration_passed = report_calibration.get("gate", {}).get("passed") is True
    report_holdout_passed = report_holdout.get("gate", {}).get("passed") is True
    if (calibration_passed, holdout_passed) != (
        report_calibration_passed,
        report_holdout_passed,
    ):
        raise ValueError("policy acceptance does not match tuning report")

    target.mkdir(parents=True)
    records = [
        _copy_record(Path(activity_model), target / "models" / "activity.joblib", "activity_model", root=target),
        _copy_record(Path(sound_model), target / "models" / "sound_event.joblib", "sound_event_model", root=target),
        _copy_record(config_path, target / "config" / "audio-policy.json", "policy_config", root=target),
        _copy_record(Path(training_metrics), target / "reports" / "training-metrics.json", "training_metrics", root=target),
        _copy_record(report_path, target / "reports" / "policy-tuning.json", "policy_tuning", root=target),
    ]
    if evaluation_report is not None:
        records.append(
            _copy_record(
                Path(evaluation_report),
                target / "reports" / "streaming-evaluation.json",
                "streaming_evaluation",
                root=target,
            )
        )
    for wheel in runtime_wheels:
        wheel_path = Path(wheel)
        records.append(
            _copy_record(
                wheel_path,
                target / "wheels" / wheel_path.name,
                "runtime_wheel",
                root=target,
            )
        )

    rollback_sources = (
        (rollback_activity_model, "activity.joblib", "rollback_activity_model"),
        (rollback_sound_model, "sound_event.joblib", "rollback_sound_event_model"),
        (rollback_metrics, "metrics.json", "rollback_metrics"),
    )
    for source, name, role in rollback_sources:
        if source is not None:
            records.append(
                _copy_record(Path(source), target / "rollback" / name, role, root=target)
            )

    blockers: list[str] = []
    if not calibration_passed:
        blockers.append("calibration_gate_failed")
    if not holdout_passed:
        blockers.append("holdout_gate_failed")
    blockers.append("cryptographic_signature_missing")
    manifest = {
        "schema_version": "1.0",
        "package_version": package_version
        or str(config.get("config_version", "audio-policy-unversioned")).replace(
            "audio-policy", "audio-context", 1
        ),
        "created_at": datetime.now(timezone.utc).isoformat(),
        "target": {"device": "Raspberry Pi 5", "architecture": "aarch64"},
        "activation": {
            "allowed": False,
            "status": "STAGING_ONLY",
            "blockers": blockers,
        },
        "acceptance": {
            "calibration_passed": calibration_passed,
            "holdout_passed": holdout_passed,
        },
        "signature": {
            "status": "UNSIGNED",
            "required_before_activation": True,
        },
        "privacy": {
            "raw_audio_included": False,
            "raw_video_included": False,
            "transcripts_included": False,
        },
        "rollback": {
            "model_version": rollback_model_version,
            "included": all(source is not None for source, _name, _role in rollback_sources),
            "procedure": "stop service, replace models from rollback/, restore prior policy, restart, verify health",
        },
        "files": records,
    }
    manifest_path = target / "package-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return manifest


def verify_release_candidate(package_dir: str | Path) -> dict[str, object]:
    """Verify every declared file without trusting paths outside the bundle."""

    root = Path(package_dir).resolve()
    manifest_path = root / "package-manifest.json"
    manifest = _read_object(manifest_path)
    files = manifest.get("files")
    if not isinstance(files, list):
        raise ValueError("package manifest files must be an array")
    results: list[dict[str, object]] = []
    for raw in files:
        if not isinstance(raw, dict) or not isinstance(raw.get("path"), str):
            raise ValueError("every package file record needs a path")
        path = (root / raw["path"]).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise ValueError(f"package path escapes bundle: {raw['path']}") from exc
        exists = path.is_file()
        actual_bytes = path.stat().st_size if exists else None
        actual_sha256 = sha256_file(path) if exists else None
        valid = (
            exists
            and actual_bytes == raw.get("bytes")
            and actual_sha256 == raw.get("sha256")
        )
        results.append({"path": raw["path"], "valid": valid})
    return {
        "schema_version": "1.0",
        "package_version": manifest.get("package_version"),
        "valid": bool(results) and all(item["valid"] for item in results),
        "checked_files": len(results),
        "results": results,
        "activation": manifest.get("activation"),
    }
