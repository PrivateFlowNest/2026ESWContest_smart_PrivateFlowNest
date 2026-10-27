"""PC-only Pi 4 policy and controller ACK simulation for integration gates.

This module deliberately stops at the hardware boundary.  It resolves only IDs
present in the user-provided spreadsheet inventory and never pretends that a
real actuator moved.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
from time import perf_counter
from typing import Any, Iterable, Mapping

from .client import ExaoneClient
from .validation import ProposalValidationError, validate_proposal


QUIET_MODE_EXPANSION = (
    ("TV", "MUTE"),
    ("WASHER", "DEFER_NOTIFICATION"),
    ("AIR_CONDITIONER", "SET_QUIET_MODE"),
    ("VENTILATION_FAN", "TURN_OFF"),
)


class PolicyRejection(ValueError):
    """Raised when a proposal cannot cross the simulated Pi 4 policy boundary."""


@dataclass(frozen=True)
class RoutedCommand:
    device: str
    hardware_id: str
    controller_id: str
    channel: str
    action: str
    parameters: dict[str, Any]


class MockControllerBus:
    """Return deterministic ACKs without claiming real hardware execution."""

    def __init__(self) -> None:
        self.executions: list[RoutedCommand] = []

    def dispatch(self, command: RoutedCommand, *, command_id: str) -> dict[str, Any]:
        self.executions.append(command)
        return {
            "schema_version": "1.0",
            "message_type": "SIMULATED_ACTUATOR_ACK",
            "command_id": command_id,
            "controller_id": command.controller_id,
            "hardware_id": command.hardware_id,
            "channel": command.channel,
            "action": command.action,
            "status": "SIMULATED_ACK",
            "real_hardware_executed": False,
        }


class Pi4PolicySimulator:
    """Mirror the minimum deterministic checks required at the Pi 4 boundary."""

    def __init__(
        self,
        capabilities: Mapping[str, Any],
        inventory: Mapping[str, Any],
        bus: MockControllerBus | None = None,
    ) -> None:
        self.capabilities = dict(capabilities)
        self.inventory = dict(inventory)
        self.bus = bus or MockControllerBus()
        self._processed: dict[str, dict[str, Any]] = {}
        self._hardware_by_id: dict[str, list[dict[str, Any]]] = {}
        for record in self.inventory.get("hardware", []):
            if isinstance(record, dict) and isinstance(record.get("id"), str):
                self._hardware_by_id.setdefault(record["id"], []).append(record)

    def _route(self, device: str, action: str, parameters: Mapping[str, Any]) -> RoutedCommand:
        devices = self.capabilities.get("devices", {})
        capability = devices.get(device)
        if not isinstance(capability, dict) or not capability.get("enabled"):
            raise PolicyRejection(f"device is disabled or unknown: {device}")
        if action not in capability.get("actions", []):
            raise PolicyRejection(f"action {action} is not allowed for {device}")
        hardware_id = capability.get("hardware_id")
        if not isinstance(hardware_id, str) or not hardware_id:
            raise PolicyRejection(f"device has no direct hardware mapping: {device}")
        records = self._hardware_by_id.get(hardware_id, [])
        if len(records) != 1:
            raise PolicyRejection(
                f"hardware ID must resolve exactly once: {hardware_id} ({len(records)} matches)"
            )
        record = records[0]
        controller = record.get("controller")
        channel = record.get("channel")
        if not isinstance(controller, str) or not controller:
            raise PolicyRejection(f"controller is not fixed in spreadsheet: {hardware_id}")
        if not isinstance(channel, str) or not channel:
            raise PolicyRejection(f"channel is not fixed in spreadsheet: {hardware_id}")
        return RoutedCommand(
            device=device,
            hardware_id=hardware_id,
            controller_id=controller,
            channel=channel,
            action=action,
            parameters=dict(parameters),
        )

    def _commands(self, proposal: Mapping[str, Any]) -> list[RoutedCommand]:
        if proposal["intent"] == "CONTEXT_SUGGESTION":
            if (
                proposal["device"], proposal["action"]
            ) != ("ALL_QUIET_DEVICES", "SET_QUIET_MODE"):
                raise PolicyRejection("unsupported context suggestion")
            return [
                self._route(device, action, {"reason": "quiet_required"})
                for device, action in QUIET_MODE_EXPANSION
            ]
        if proposal["intent"] != "DEVICE_CONTROL":
            raise PolicyRejection(f"intent is not executable: {proposal['intent']}")
        return [
            self._route(
                str(proposal["device"]),
                str(proposal["action"]),
                proposal["parameters"],
            )
        ]

    def process(self, proposal_id: str, raw_proposal: Mapping[str, Any]) -> dict[str, Any]:
        if proposal_id in self._processed:
            cached = dict(self._processed[proposal_id])
            cached["duplicate"] = True
            return cached
        try:
            proposal = validate_proposal(
                raw_proposal, capabilities=self.capabilities
            )
        except ProposalValidationError as exc:
            raise PolicyRejection("; ".join(exc.errors)) from exc
        if proposal["ambiguous"] or proposal["safety_flags"]:
            raise PolicyRejection("ambiguous or safety-flagged proposal")
        commands = self._commands(proposal)
        acknowledgements = [
            self.bus.dispatch(command, command_id=f"{proposal_id}:{index}")
            for index, command in enumerate(commands, start=1)
        ]
        result = {
            "proposal_id": proposal_id,
            "status": "SIMULATED_ACK",
            "duplicate": False,
            "real_hardware_executed": False,
            "acknowledgements": acknowledgements,
        }
        self._processed[proposal_id] = result
        return dict(result)


def load_e2e_cases(path: str | Path) -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    with Path(path).open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"case line {line_number} must be an object")
            cases.append(value)
    if not cases:
        raise ValueError("at least one E2E case is required")
    return cases


def run_e2e_gate(
    client: ExaoneClient,
    cases: Iterable[Mapping[str, Any]],
    *,
    capabilities: Mapping[str, Any],
    inventory: Mapping[str, Any],
) -> dict[str, Any]:
    bus = MockControllerBus()
    policy = Pi4PolicySimulator(capabilities, inventory, bus)
    results: list[dict[str, Any]] = []
    consecutive = 0
    for index, case in enumerate(cases, start=1):
        started = perf_counter()
        result = client.interpret(
            str(case["utterance"]), context=dict(case.get("context", {}))
        )
        proposal_id = f"pc-e2e-{index:03d}"
        errors: list[str] = []
        ack: dict[str, Any] | None = None
        duplicate: dict[str, Any] | None = None
        if not result.accepted:
            errors.append("EXAONE final proposal was not accepted")
        else:
            try:
                ack = policy.process(proposal_id, result.proposal)
                duplicate = policy.process(proposal_id, result.proposal)
            except PolicyRejection as exc:
                errors.append(str(exc))
        expected = case.get("expected_routes", [])
        actual = [
            {"hardware_id": item["hardware_id"], "action": item["action"]}
            for item in (ack or {}).get("acknowledgements", [])
        ]
        if actual != expected:
            errors.append(f"route mismatch: expected {expected!r}, got {actual!r}")
        if ack and ack.get("real_hardware_executed") is not False:
            errors.append("simulation incorrectly claimed real hardware execution")
        if duplicate and not duplicate.get("duplicate"):
            errors.append("idempotency duplicate was not detected")
        passed = not errors
        consecutive = consecutive + 1 if passed else 0
        results.append(
            {
                "case_id": case.get("id", proposal_id),
                "utterance": case["utterance"],
                "passed": passed,
                "errors": errors,
                "model_origin": result.origin,
                "model_output_valid": result.model_output_valid,
                "proposal": result.proposal,
                "routes": actual,
                "idempotency_verified": bool(duplicate and duplicate.get("duplicate")),
                "latency_ms": round((perf_counter() - started) * 1000, 3),
            }
        )
    passed_count = sum(bool(item["passed"]) for item in results)
    return {
        "schema_version": "1.0",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "test_mode": "PC_POLICY_AND_CONTROLLER_SIMULATION",
        "real_hardware_executed": False,
        "spreadsheet_inventory_source": inventory.get("source"),
        "total": len(results),
        "passed": passed_count,
        "success_rate": passed_count / len(results),
        "final_consecutive_successes": consecutive,
        "gate_required": 10,
        "gate_passed": consecutive >= 10,
        "simulated_controller_executions": len(bus.executions),
        "results": results,
    }
