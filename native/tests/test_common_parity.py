"""hm_common vs services/runtime (Python reference): action map parity and pfn.v1 contract validity."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import CONTRACTS, build, ctest, run_probe, validate, validate_topic_message  # noqa: E402

from homemind_runtime import pfn as py_pfn  # noqa: E402
from homemind_runtime.action_map import ActionMap  # noqa: E402

PROBE = "libs/hm_common/hm_common_probe"


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build(target="hm_common_tests hm_common_probe")


def _proposals() -> list[dict]:
    action_map = json.loads((CONTRACTS / "pfn.v1" / "ai-action-map.json").read_text(encoding="utf-8"))
    devices = sorted({item["ai_device"] for item in action_map["actions"]} | set(action_map["devices"]) | {"ROBOT"})
    actions = sorted({item["ai_action"] for item in action_map["actions"]} | {"FLY"})
    out = []
    for intent in ("DEVICE_CONTROL", "CONTEXT_SUGGESTION", "DEVICE_QUERY", "CLARIFY", "NO_ACTION"):
        for device in devices + [None]:
            for action in actions + [None]:
                for parameters in ({}, {"temperature_c": 24, "level": 2, "volume_delta": 1}):
                    out.append({"intent": intent, "device": device, "action": action, "parameters": parameters,
                                "ambiguous": False, "safety_flags": [], "clarification_question": None})
    return out


def test_unit_tests_pass() -> None:
    ctest(regex="^hm_common_tests$")


def test_action_map_parity() -> None:
    proposals = _proposals()
    reference = ActionMap(CONTRACTS)
    answers = run_probe(PROBE, [{"op": "resolve", "proposal": p} for p in proposals])
    mismatches = []
    for proposal, native in zip(proposals, answers):
        try:
            py = reference.resolve(proposal)
        except KeyError as error:  # Python raises on unregistered position refs; C++ reports unsupported
            py = None
            if native["kind"] != "unsupported":
                mismatches.append((proposal, "python KeyError", error, native))
            continue
        expected = {"kind": py.kind, "device_id": py.device_id, "command": py.command, "parameters": py.parameters,
                    "intent": py.intent, "reason": py.reason, "door_open": reference.is_door_open(py)}
        if expected != native:
            mismatches.append((proposal, expected, native))
    assert not mismatches, json.dumps(mismatches[:5], ensure_ascii=False, default=str, indent=1)
    assert len(proposals) > 1000


def test_builders_are_contract_valid() -> None:
    request = {"op": "llm_request", "args": {"text": "아 좀 덥네", "stt_confidence": 0.8, "simulated": True,
                                             "context": {"presence_zone": "DESK", "activity": "MEETING",
                                                         "audio_context": "ZOOM_SPEAKER", "quiet_required": True,
                                                         "confidence": 0.9}}}
    [llm] = run_probe(PROBE, [request])
    validate_topic_message(py_pfn.TOPICS["llm_request"], llm)
    proposal = {"intent": "DEVICE_CONTROL", "device": "AIR_CONDITIONER", "action": "TURN_ON", "parameters": {},
                "ambiguous": False, "safety_flags": [], "clarification_question": None}
    resolution = ActionMap(CONTRACTS).resolve(proposal)
    from homemind_runtime.action_map import control_parameters

    lines = [
        {"op": "task_request", "args": {"source_node_id": "NODE-RPI5-01", "intent": "device_control",
                                        "target_device": resolution.device_id, "confidence": 0.7,
                                        "parameters": control_parameters(resolution, origin="llm", proposal=proposal)}},
        {"op": "task_request", "args": {"source_node_id": "NODE-RPI5-02", "intent": "prepare_home",
                                        "target_device": "HOME-ENV-01",
                                        "parameters": {"origin": "voice_parser", "ai_device": None, "ai_action": None}}},
        {"op": "llm_result", "request": llm, "args": {"status": "task_requested", "task_id": py_pfn.new_id(),
                                                      "proposal": proposal, "origin": "model", "latency_ms": 812.5,
                                                      "model_name": "exaone-1.2b-homemind-v1", "simulated": True}},
        {"op": "llm_result", "request": llm, "args": {"status": "model_unavailable", "proposal": None,
                                                      "origin": "none", "model_name": "m"}},
        {"op": "perception", "args": {"observation_type": "ACTIVITY_CONTEXT", "zone": "desk", "confidence": 0.91,
                                      "simulated": True,
                                      "attributes": {"activity": "MEETING", "audio_context": "ZOOM_SPEAKER",
                                                     "quiet_required": True, "transition": "ACTIVATED",
                                                     "evidence": ["AUDIO_CONTEXT:ZOOM_SPEAKER"],
                                                     "valid_until_ms": py_pfn.now_ms() + 15000,
                                                     "model_version": "v6"}}},
        {"op": "check_llm_request", "request": llm},
    ]
    task, scenario, result, failure, perception, check = run_probe(PROBE, lines)
    validate_topic_message(py_pfn.TOPICS["task_request"], task)
    validate_topic_message(py_pfn.TOPICS["task_request"], scenario)
    validate_topic_message(py_pfn.TOPICS["llm_result"], result)
    validate_topic_message(py_pfn.TOPICS["llm_result"], failure)
    validate_topic_message(py_pfn.TOPICS["perception"], perception)
    assert check == {"problem": ""}
    assert result["trace_id"] == llm["trace_id"] and result["ai_device"] == "AIR_CONDITIONER"
    # Python builder produces the same keys
    py_task = py_pfn.task_request(source_node_id="NODE-RPI5-01", intent="device_control",
                                  target_device=resolution.device_id, confidence=0.7,
                                  parameters=control_parameters(resolution, origin="llm", proposal=proposal))
    assert set(py_task) == set(task)
    assert {k: v for k, v in py_task.items() if k not in {"event_id", "trace_id", "task_id", "requested_at_ms"}} == \
           {k: v for k, v in task.items() if k not in {"event_id", "trace_id", "task_id", "requested_at_ms"}}
    validate(task["parameters"], "device-control.schema.json")
