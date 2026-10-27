import json
from pathlib import Path

import pytest

from homemind_exaone.deployment import build_release_candidate, verify_release_candidate


def write(path: Path, value):
    if isinstance(value, dict):
        path.write_text(json.dumps(value), encoding="utf-8")
    else:
        path.write_bytes(value)
    return path


def reports(tmp_path):
    acceptance = write(
        tmp_path / "acceptance.json",
        {
            "total": 100,
            "contract_success_rate": 1.0,
            "expectation_success_rate": 1.0,
            "model_first_pass_success_rate": 1.0,
            "category_metrics": {
                "safety": {"expectation_success_rate": 1.0}
            },
        },
    )
    e2e = write(
        tmp_path / "e2e.json",
        {"gate_passed": True, "final_consecutive_successes": 10},
    )
    return acceptance, e2e


def test_build_and_verify_staging_bundle(tmp_path):
    acceptance, e2e = reports(tmp_path)
    sources = {
        "model": write(tmp_path / "model.gguf", b"model"),
        "capabilities": write(tmp_path / "capabilities.json", {}),
        "inventory": write(tmp_path / "inventory.json", {}),
        "schema": write(tmp_path / "schema.json", {}),
        "bridge": write(tmp_path / "bridge.jsonl", b"{}\n"),
    }
    output = tmp_path / "release"
    manifest = build_release_candidate(
        output,
        model=sources["model"], capabilities=sources["capabilities"],
        inventory=sources["inventory"], schemas=[sources["schema"]],
        acceptance_report=acceptance, e2e_report=e2e,
        bridge_report=sources["bridge"],
    )
    assert manifest["activation"]["status"] == "STAGING_ONLY"
    assert manifest["privacy"]["raw_audio_included"] is False
    assert verify_release_candidate(output)["valid"] is True


def test_failed_acceptance_cannot_be_packaged(tmp_path):
    acceptance, e2e = reports(tmp_path)
    value = json.loads(acceptance.read_text())
    value["expectation_success_rate"] = 0.94
    acceptance.write_text(json.dumps(value), encoding="utf-8")
    source = write(tmp_path / "source", b"x")
    with pytest.raises(ValueError, match="acceptance"):
        build_release_candidate(
            tmp_path / "release", model=source, capabilities=source,
            inventory=source, schemas=[source], acceptance_report=acceptance,
            e2e_report=e2e, bridge_report=source,
        )


def test_verifier_detects_tampering(tmp_path):
    acceptance, e2e = reports(tmp_path)
    source = write(tmp_path / "source", b"x")
    output = tmp_path / "release"
    build_release_candidate(
        output, model=source, capabilities=source, inventory=source,
        schemas=[source], acceptance_report=acceptance, e2e_report=e2e,
        bridge_report=source,
    )
    (output / "models" / "exaone-1.2b-q4_k_m.gguf").write_bytes(b"tampered")
    assert verify_release_candidate(output)["valid"] is False
