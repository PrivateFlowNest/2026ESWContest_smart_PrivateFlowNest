from __future__ import annotations

import json

import pytest

from homemind_context_fusion.models import ContextObservation
from homemind_context_fusion.policy_config import ContextFusionPolicy


def test_policy_config_loads_nested_engine_and_builds_engine(tmp_path):
    path = tmp_path / "policy.json"
    path.write_text(
        json.dumps(
            {
                "schema_version": "1.0",
                "engine": {
                    "quiet_audio_confidence": 0.65,
                    "activation_updates": 4,
                    "release_updates": 2,
                    "change_updates": 2,
                    "ttl_seconds": 10.0,
                },
            }
        ),
        encoding="utf-8",
    )

    policy = ContextFusionPolicy.from_json(path)
    engine = policy.engine()

    assert engine.quiet_audio_confidence == 0.65
    assert engine.activation_updates == 4


def test_policy_config_rejects_unknown_engine_fields():
    with pytest.raises(ValueError, match="unknown"):
        ContextFusionPolicy.from_mapping({"engine": {"unsafe_override": True}})


def test_policy_threshold_suppresses_low_confidence_meeting():
    engine = ContextFusionPolicy(quiet_audio_confidence=0.7).engine()
    event = engine.update(
        ContextObservation(
            audio_context="IN_PERSON_MEETING",
            audio_confidence=0.6,
            speech_ratio=0.5,
        )
    )

    assert event["quiet_required"] is False


def test_policy_config_rejects_failed_acceptance_for_runtime(tmp_path):
    path = tmp_path / "rejected.json"
    path.write_text(
        json.dumps(
            {
                "engine": {"quiet_audio_confidence": 0.6},
                "acceptance": {
                    "calibration_passed": False,
                    "holdout_passed": False,
                },
            }
        ),
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="not accepted"):
        ContextFusionPolicy.from_json(path)
    assert ContextFusionPolicy.from_json(path, require_accepted=False).quiet_audio_confidence == 0.6
