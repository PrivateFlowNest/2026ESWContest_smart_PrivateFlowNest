"""Build and verify a hash-pinned EXAONE Raspberry Pi 5 staging bundle."""

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


def _read_object(path: str | Path) -> dict[str, object]:
    source = Path(path)
    value = json.loads(source.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{source} must contain a JSON object")
    return value


def _copy_record(source: Path, destination: Path, role: str, root: Path) -> dict[str, object]:
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
    model: str | Path,
    capabilities: str | Path,
    inventory: str | Path,
    schemas: Iterable[str | Path],
    acceptance_report: str | Path,
    e2e_report: str | Path,
    bridge_report: str | Path | None = None,
    runtime_wheel: str | Path | None = None,
    package_version: str = "exaone-edge-v1",
) -> dict[str, object]:
    target = Path(output_dir)
    if target.exists():
        raise FileExistsError(f"release destination already exists: {target}")

    acceptance = _read_object(acceptance_report)
    e2e = _read_object(e2e_report)
    acceptance_passed = (
        acceptance.get("contract_success_rate") == 1.0
        and acceptance.get("expectation_success_rate", 0.0) >= 0.95
        and acceptance.get("model_first_pass_success_rate", 0.0) >= 0.95
        and isinstance(acceptance.get("category_metrics"), dict)
        and acceptance["category_metrics"].get("safety", {}).get(
            "expectation_success_rate"
        ) == 1.0
    )
    e2e_passed = e2e.get("gate_passed") is True and e2e.get(
        "final_consecutive_successes", 0
    ) >= 10
    if not acceptance_passed:
        raise ValueError("EXAONE acceptance report does not satisfy release thresholds")
    if not e2e_passed:
        raise ValueError("PC E2E report does not contain 10 consecutive successes")

    target.mkdir(parents=True)
    records = [
        _copy_record(Path(model), target / "models" / "exaone-1.2b-q4_k_m.gguf", "q4_k_m_model", target),
        _copy_record(Path(capabilities), target / "config" / "device-capabilities.json", "device_capabilities", target),
        _copy_record(Path(inventory), target / "config" / "hardware-inventory.json", "spreadsheet_hardware_inventory", target),
        _copy_record(Path(acceptance_report), target / "reports" / "acceptance.json", "fresh_acceptance_report", target),
        _copy_record(Path(e2e_report), target / "reports" / "pc-e2e.json", "pc_e2e_report", target),
    ]
    if bridge_report is not None:
        # Legacy quiet-transition HTTP bridge; superseded by perception ACTIVITY_CONTEXT -> core.
        records.append(
            _copy_record(Path(bridge_report), target / "reports" / "audio-bridge-events.jsonl", "audio_bridge_report", target)
        )
    for schema in schemas:
        schema_path = Path(schema)
        records.append(
            _copy_record(schema_path, target / "contracts" / schema_path.name, "json_schema", target)
        )
    if runtime_wheel is not None:
        wheel = Path(runtime_wheel)
        records.append(
            _copy_record(wheel, target / "wheels" / wheel.name, "runtime_wheel", target)
        )

    manifest = {
        "schema_version": "1.0",
        "package_version": package_version,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "target": {
            "node_id": "NODE-RPI5-01",
            "device": "Raspberry Pi 5",
            "architecture": "aarch64",
            "model_runtime": "llama.cpp",
            "concurrency": 1,
        },
        "activation": {
            "allowed": False,
            "status": "STAGING_ONLY",
            "blockers": [
                "cryptographic_signature_missing",
                "pi5_latency_and_thermal_test_pending",
                "real_pi4_esp_ack_test_pending",
            ],
        },
        "acceptance": {
            "fresh_cases": acceptance.get("total"),
            "contract_success_rate": acceptance.get("contract_success_rate"),
            "expectation_success_rate": acceptance.get("expectation_success_rate"),
            "model_first_pass_success_rate": acceptance.get(
                "model_first_pass_success_rate"
            ),
            "pc_e2e_consecutive_successes": e2e.get(
                "final_consecutive_successes"
            ),
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
            "mode": "GUARD_ONLY_SAFE_FALLBACK",
            "procedure": "stop llama.cpp, keep deterministic safety guards active, return CLARIFY for commands without a guard, restore previous signed bundle, verify health",
        },
        "files": records,
    }
    (target / "package-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def verify_release_candidate(package_dir: str | Path) -> dict[str, object]:
    root = Path(package_dir).resolve()
    manifest = _read_object(root / "package-manifest.json")
    files = manifest.get("files")
    if not isinstance(files, list):
        raise ValueError("package manifest files must be an array")
    results: list[dict[str, object]] = []
    for record in files:
        if not isinstance(record, dict) or not isinstance(record.get("path"), str):
            raise ValueError("invalid package file record")
        path = (root / record["path"]).resolve()
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise ValueError(f"package path escapes bundle: {record['path']}") from exc
        valid = (
            path.is_file()
            and path.stat().st_size == record.get("bytes")
            and sha256_file(path) == record.get("sha256")
        )
        results.append({"path": record["path"], "valid": valid})
    return {
        "schema_version": "1.0",
        "package_version": manifest.get("package_version"),
        "valid": bool(results) and all(item["valid"] for item in results),
        "checked_files": len(results),
        "activation": manifest.get("activation"),
        "results": results,
    }
