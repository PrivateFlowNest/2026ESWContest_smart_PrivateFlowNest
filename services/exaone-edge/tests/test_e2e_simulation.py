import json
from pathlib import Path

import pytest

from homemind_exaone.client import InterpretationResult
from homemind_exaone.e2e_simulation import (
    MockControllerBus,
    Pi4PolicySimulator,
    PolicyRejection,
    run_e2e_gate,
)


ROOT = Path(__file__).resolve().parents[1]


def load(name):
    return json.loads((ROOT / name).read_text(encoding="utf-8"))


def proposal(device="TV", action="MUTE", intent="DEVICE_CONTROL"):
    return {
        "intent": intent,
        "device": device,
        "action": action,
        "parameters": {},
        "ambiguous": False,
        "safety_flags": [],
        "clarification_question": None,
    }


class FakeClient:
    model = "exaone-test"

    def interpret(self, utterance, *, context):
        return InterpretationResult(
            proposal=proposal(), accepted=True, origin="model_guard",
            model_output_valid=True, attempts=1, latency_ms=1.0,
            model=self.model, raw_content=None, errors=(),
        )


def policy():
    return Pi4PolicySimulator(
        load("config/device-capabilities.json"),
        load("config/hardware-inventory.json"),
    )


def test_policy_resolves_exact_spreadsheet_id_and_deduplicates():
    target = policy()
    first = target.process("p-1", proposal())
    duplicate = target.process("p-1", proposal())
    assert first["acknowledgements"][0]["hardware_id"] == "LED-TV-01"
    assert first["real_hardware_executed"] is False
    assert duplicate["duplicate"] is True
    assert len(target.bus.executions) == 1


def test_light_routes_to_living_led_on_d7_controller():
    result = policy().process("p-light", proposal("LIGHT", "TURN_ON"))
    acknowledgement = result["acknowledgements"][0]
    assert acknowledgement["hardware_id"] == "LED-LIVING-01"
    assert result["real_hardware_executed"] is False


def test_unassigned_controller_is_not_invented():
    inventory = load("config/hardware-inventory.json")
    for item in inventory["hardware"]:
        if item["id"] == "LED-TV-01":
            item["controller"] = None
    target = Pi4PolicySimulator(load("config/device-capabilities.json"), inventory)
    with pytest.raises(PolicyRejection, match="controller is not fixed"):
        target.process("p-tv", proposal())


def test_duplicate_hardware_id_is_rejected():
    inventory = load("config/hardware-inventory.json")
    duplicate = dict(next(item for item in inventory["hardware"] if item["id"] == "LED-TV-01"))
    inventory["hardware"].append(duplicate)
    target = Pi4PolicySimulator(load("config/device-capabilities.json"), inventory)
    with pytest.raises(PolicyRejection, match="exactly once"):
        target.process("p-dup", proposal())


def test_ten_consecutive_gate_requires_all_ten():
    case = {
        "utterance": "TV 음소거해줘",
        "expected_routes": [{"hardware_id": "LED-TV-01", "action": "MUTE"}],
    }
    report = run_e2e_gate(
        FakeClient(), [case] * 10,
        capabilities=load("config/device-capabilities.json"),
        inventory=load("config/hardware-inventory.json"),
    )
    assert report["gate_passed"] is True
    assert report["final_consecutive_successes"] == 10
    assert report["real_hardware_executed"] is False
