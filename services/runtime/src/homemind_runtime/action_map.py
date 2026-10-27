"""AI task proposal -> core request, using contracts/pfn.v1/ai-action-map.json and device-registry.json."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping

from .config import CONTRACTS_DIR


@dataclass(frozen=True)
class Resolution:
    kind: str  # device_control | scenario | query | unsupported | none
    device_id: str | None = None
    command: str | None = None
    parameters: dict[str, Any] = field(default_factory=dict)
    intent: str | None = None
    reason: str | None = None


class ActionMap:
    def __init__(self, contracts_dir: Path = CONTRACTS_DIR) -> None:
        pfn = contracts_dir / "pfn.v1"
        self.map = json.loads((pfn / "ai-action-map.json").read_text(encoding="utf-8"))
        registry = json.loads((pfn / "device-registry.json").read_text(encoding="utf-8"))
        self.devices = {item["device_id"]: item for item in registry["devices"]}
        self.zone_map: dict[str, str] = self.map["zone_map"]
        self._actions = {(item["ai_device"], item["ai_action"]): item for item in self.map["actions"]}

    def zone(self, ai_zone: str | None) -> str:
        return self.zone_map.get(ai_zone or "UNKNOWN", "unknown")

    def resolve(self, proposal: Mapping[str, Any]) -> Resolution:
        intent = proposal.get("intent")
        if intent not in {"DEVICE_CONTROL", "CONTEXT_SUGGESTION", "DEVICE_QUERY"}:
            return Resolution("none", reason=str(intent))
        ai_device = proposal.get("device")
        ai_action = proposal.get("action")
        entry = self._actions.get((ai_device, ai_action)) or self._actions.get(("*", ai_action))
        if entry is None:
            return Resolution("unsupported", reason=f"no mapping for {ai_device}/{ai_action}")
        if not entry["enabled"]:
            return Resolution("unsupported", reason=entry.get("pending_decision", "disabled"))
        if entry["kind"] == "query":
            return Resolution("query", device_id=self._device_id(ai_device, "device_id"))
        if entry["kind"] == "scenario":
            return Resolution("scenario", intent=entry["intent"])
        if entry["kind"] != "device_control":
            return Resolution("unsupported", reason=entry["kind"])
        device_entry = self.map["devices"].get(ai_device, {})
        if not device_entry.get("enabled"):
            return Resolution("unsupported", reason=device_entry.get("reason", "device disabled"))
        device_id = self._device_id(ai_device, entry.get("target", "device_id"))
        if device_id is None:
            return Resolution("unsupported", reason="no registry device")
        parameters = dict(entry.get("parameters", {}))
        if "position_ref" in entry:
            positions = self.devices[device_id].get("positions")
            if not positions:
                return Resolution("unsupported", reason="no registered positions")
            parameters["position_degrees"] = positions[entry["position_ref"]]
        for source, target in entry.get("parameter_from", {}).items():
            value = (proposal.get("parameters") or {}).get(source)
            if value is None:
                return Resolution("unsupported", reason=f"missing parameter {source}")
            parameters[target] = value
        return Resolution("device_control", device_id=device_id, command=entry["command"], parameters=parameters)

    def _device_id(self, ai_device: str | None, key: str) -> str | None:
        entry = self.map["devices"].get(ai_device or "", {})
        value = entry.get(key)
        return value if isinstance(value, str) else None

    def is_door_open(self, resolution: Resolution) -> bool:
        if resolution.kind != "device_control" or resolution.device_id is None:
            return False
        positions = self.devices[resolution.device_id].get("positions") or {}
        return resolution.command == "set_position" and resolution.parameters.get("position_degrees") == positions.get("open")


def control_parameters(resolution: Resolution, *, origin: str, proposal: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "target_device_id": resolution.device_id,
        "action": resolution.command,
        "parameters": resolution.parameters,
        "origin": origin,
        "ai_device": proposal.get("device"),
        "ai_action": proposal.get("action"),
    }


def scenario_parameters(*, origin: str, proposal: Mapping[str, Any]) -> dict[str, Any]:
    return {"origin": origin, "ai_device": proposal.get("device"), "ai_action": proposal.get("action")}
