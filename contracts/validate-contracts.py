#!/usr/bin/env python3
"""Dependency-free checks for HomeMind JSON contracts and shared IDs.

Canonical contracts live in this directory:
  pfn.v1/          MQTT contracts between nodes (team pfn.v1, extended)
  rpi5-internal/   AI events that never leave the RPI5 nodes

Run: python contracts/validate-contracts.py
"""

from __future__ import annotations

import ipaddress
import json
import re
import sys
import uuid
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent
PFN = ROOT / "pfn.v1"
INTERNAL = ROOT / "rpi5-internal"


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def load_schema(path: Path) -> Any:
    """Load a schema with "$ref" inlined (local "#/$defs/..." and sibling files)."""
    root = load_json(path)

    def resolve(node: Any, document: Any, base: Path, depth: int) -> Any:
        if depth > 20:
            raise ValueError(f"{path.name}: $ref nesting is too deep")
        if isinstance(node, list):
            return [resolve(item, document, base, depth) for item in node]
        if not isinstance(node, dict):
            return node
        if "$ref" in node:
            reference = node["$ref"]
            file_part, _, pointer = reference.partition("#")
            target_document = load_json(base / file_part) if file_part else document
            target = target_document
            for key in [part for part in pointer.split("/") if part]:
                target = target[key]
            merged = {k: v for k, v in node.items() if k != "$ref"}
            resolved = resolve(target, target_document, base, depth + 1)
            return {**resolved, **merged} if merged else resolved
        return {key: resolve(value, document, base, depth) for key, value in node.items() if key != "$defs"}

    return resolve(root, root, path.parent, 0)


def is_type(value: Any, expected: str) -> bool:
    return {
        "object": isinstance(value, dict),
        "array": isinstance(value, list),
        "string": isinstance(value, str),
        "integer": isinstance(value, int) and not isinstance(value, bool),
        "number": isinstance(value, (int, float)) and not isinstance(value, bool),
        "boolean": isinstance(value, bool),
        "null": value is None,
    }.get(expected, True)


def validate(instance: Any, schema: dict[str, Any], path: str = "$") -> list[str]:
    errors: list[str] = []

    if "oneOf" in schema:
        matches = sum(not validate(instance, item, path) for item in schema["oneOf"])
        if matches != 1:
            errors.append(f"{path}: oneOf matched {matches} branches")
            return errors

    expected = schema.get("type")
    if expected is not None:
        expected_types = expected if isinstance(expected, list) else [expected]
        if not any(is_type(instance, item) for item in expected_types):
            errors.append(f"{path}: expected {expected_types}, got {type(instance).__name__}")
            return errors

    if "const" in schema and instance != schema["const"]:
        errors.append(f"{path}: expected constant {schema['const']!r}")
    if "enum" in schema and instance not in schema["enum"]:
        errors.append(f"{path}: {instance!r} is not in enum")

    if isinstance(instance, str):
        if len(instance) < schema.get("minLength", 0):
            errors.append(f"{path}: string is too short")
        if len(instance) > schema.get("maxLength", sys.maxsize):
            errors.append(f"{path}: string is too long")
        if "pattern" in schema and re.fullmatch(schema["pattern"], instance) is None:
            errors.append(f"{path}: does not match {schema['pattern']}")
        if schema.get("format") == "uuid":
            try:
                uuid.UUID(instance)
            except ValueError:
                errors.append(f"{path}: invalid UUID")
        if schema.get("format") == "ipv4":
            try:
                if ipaddress.ip_address(instance).version != 4:
                    raise ValueError
            except ValueError:
                errors.append(f"{path}: invalid IPv4 address")

    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if instance < schema.get("minimum", instance):
            errors.append(f"{path}: below minimum")
        if instance > schema.get("maximum", instance):
            errors.append(f"{path}: above maximum")

    if isinstance(instance, list):
        if len(instance) < schema.get("minItems", 0):
            errors.append(f"{path}: too few items")
        if schema.get("uniqueItems"):
            encoded = [json.dumps(item, sort_keys=True) for item in instance]
            if len(encoded) != len(set(encoded)):
                errors.append(f"{path}: items are not unique")
        if isinstance(schema.get("items"), dict):
            for index, item in enumerate(instance):
                errors.extend(validate(item, schema["items"], f"{path}[{index}]"))

    if isinstance(instance, dict):
        for key in schema.get("required", []):
            if key not in instance:
                errors.append(f"{path}: missing required property {key}")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, value in instance.items():
            if key in properties:
                errors.extend(validate(value, properties[key], f"{path}.{key}"))
            elif additional is False:
                errors.append(f"{path}: unexpected property {key}")
            elif isinstance(additional, dict):
                errors.extend(validate(value, additional, f"{path}.{key}"))

    for branch in schema.get("allOf", []):
        condition = branch.get("if")
        if condition is None:
            errors.extend(validate(instance, branch, path))
        elif not validate(instance, condition, path):
            errors.extend(validate(instance, branch.get("then", {}), path))
        else:
            errors.extend(validate(instance, branch.get("else", {}), path))

    return errors



class Checker:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.checked_examples = 0

    def error(self, message: str) -> None:
        self.errors.append(message)

    def schema_check(self, label: str, instance: Any, schema: dict[str, Any]) -> bool:
        problems = validate(instance, schema)
        for problem in problems:
            self.error(f"{label}: {problem}")
        return not problems


def pfn(name: str) -> Any:
    return load_json(PFN / name)


# Example file -> schema. Every file under examples/ must be listed.
PFN_EXAMPLES = {
    "door-command.json": "command.schema.json",
    "door-ack.json": "ack.schema.json",
    "safety-alert.json": "safety-alert.schema.json",
    "privacy-state.json": "privacy-state.schema.json",
    "state-response-invalid-clock.json": "state-response.schema.json",
    "llm-request.json": "llm-request.schema.json",
    "llm-result-task.json": "llm-result.schema.json",
    "llm-result-clarify.json": "llm-result.schema.json",
    "task-request-device-control.json": "task-request.schema.json",
    "task-request-voice-door.json": "task-request.schema.json",
    "task-request-quiet-mode.json": "task-request.schema.json",
    "perception-figure-presence.json": "perception.schema.json",
    "perception-activity-context.json": "perception.schema.json",
    "perception-partial-intent.json": "perception.schema.json",
    "voice-listen-start.json": "voice-listen.schema.json",
    "mobile-pairing-request.json": "mobile-pairing-request.schema.json",
    "mobile-pairing-offer.json": "mobile-pairing-offer.schema.json",
    "mobile-phones-request.json": "mobile-phones-request.schema.json",
    "mobile-phones-state.json": "mobile-phones-state.schema.json",
    "task-update-rejected.json": "task-update.schema.json",
}

INTERNAL_EXAMPLES = {
    "speech-transcript.json": "speech-transcript.schema.json",
    "partial-intent.json": "partial-intent.schema.json",
    "audio-context.json": "audio-context.schema.json",
    "context-feature-observation.json": "context-feature-observation.schema.json",
    "context-activity-event.json": "context-activity-event.schema.json",
    "activity-context.json": "activity-context.schema.json",
    "task-proposal.json": "task-proposal.schema.json",
}


def check_json_files(c: Checker) -> None:
    for path in sorted(ROOT.rglob("*.json")):
        try:
            load_json(path)
        except Exception as error:  # noqa: BLE001 - command-line audit
            c.error(f"{path.relative_to(ROOT)}: invalid JSON: {error}")


def check_examples(c: Checker, directory: Path, pairs: dict[str, str], schema_dir: Path) -> dict[str, Any]:
    loaded: dict[str, Any] = {}
    present = {path.name for path in directory.glob("*.json")} if directory.is_dir() else set()
    for name in sorted(present - set(pairs)):
        c.error(f"{directory.relative_to(ROOT).as_posix()}/{name}: example has no schema mapping")
    for name, schema_name in pairs.items():
        path = directory / name
        if not path.is_file():
            c.error(f"{directory.relative_to(ROOT).as_posix()}/{name}: mapped example is missing")
            continue
        document = load_json(path)
        c.checked_examples += 1
        if c.schema_check(f"{directory.relative_to(ROOT).as_posix()}/{name}", document, load_schema(schema_dir / schema_name)):
            loaded[name] = document
    return loaded


def command_errors(action: str, parameters: Any) -> list[str]:
    """Validate action parameters with command.schema.json by building a dummy command."""
    command = {
        "schema_version": "pfn.v1",
        "event_id": str(uuid.UUID(int=1)),
        "trace_id": str(uuid.UUID(int=2)),
        "command_id": str(uuid.UUID(int=3)),
        "target_node_id": "X",
        "target_device_id": "X",
        "action": action,
        "parameters": parameters,
        "issued_at_ms": 1,
        "expires_at_ms": 2,
        "attempt": 1,
    }
    return validate(command, pfn("command.schema.json"))


def check_device_action(
    c: Checker, label: str, device: dict[str, Any] | None, action: str, parameters: Any
) -> None:
    """Rules core applies before sending a command to a registry device."""
    if device is None:
        c.error(f"{label}: target device is not registered")
        return
    if device["wiring_status"] != "wired":
        c.error(f"{label}: target device {device['device_id']} is {device['wiring_status']}")
    if action not in device["capabilities"]:
        c.error(f"{label}: {action} is not a capability of {device['device_id']}")
    for problem in command_errors(action, parameters):
        c.error(f"{label}: command parameters: {problem}")
    if action == "set_color" and isinstance(parameters, dict):
        colors = device.get("supported_colors")
        if colors is not None and parameters.get("color") not in colors:
            c.error(f"{label}: colour {parameters.get('color')!r} not supported by {device['device_id']}")
    if action == "set_position" and isinstance(parameters, dict):
        positions = device.get("positions")
        if positions is None:
            c.error(f"{label}: {device['device_id']} has no registered positions")
        elif parameters.get("position_degrees") not in (positions["open"], positions["closed"]):
            c.error(f"{label}: position {parameters.get('position_degrees')} is not a registered open/closed position")


def check_registry(c: Checker, registry: dict[str, Any]) -> None:
    node_ids = [item["node_id"] for item in registry["nodes"]]
    device_ids = [item["device_id"] for item in registry["devices"]]
    if len(node_ids) != len(set(node_ids)):
        c.error("device-registry.json: duplicate node_id")
    if len(device_ids) != len(set(device_ids)):
        c.error("device-registry.json: duplicate device_id")
    zones = set(registry["zones"])
    for device in registry["devices"]:
        label = f"device-registry.json: {device['device_id']}"
        controller = device["controller_node_id"]
        if controller is not None and controller not in node_ids:
            c.error(f"{label}: unknown controller {controller}")
        if device["zone"] not in zones:
            c.error(f"{label}: zone {device['zone']} is not in zones")
        if "positions" in device:
            if "set_position" not in device["capabilities"]:
                c.error(f"{label}: positions without set_position capability")
            if device["positions"]["open"] == device["positions"]["closed"]:
                c.error(f"{label}: open and closed positions are equal")
        if "set_position" in device["capabilities"] and device["wiring_status"] == "wired" and "positions" not in device:
            c.error(f"{label}: wired servo needs positions")
        if "supported_colors" in device and "set_color" not in device["capabilities"]:
            c.error(f"{label}: supported_colors without set_color capability")
        if device["wiring_status"] == "wired" and "pin" not in device:
            c.error(f"{label}: wired device needs pin")
    pins = [(d["controller_node_id"], d["pin"]) for d in registry["devices"] if d["wiring_status"] == "wired" and "pin" in d and d["pin"] not in {"USB"}]
    if len(pins) != len(set(pins)):
        c.error("device-registry.json: two wired devices share a controller pin")


def check_door_pair(c: Checker, devices: dict[str, Any], examples: dict[str, Any]) -> None:
    command = examples["door-command.json"]
    ack = examples["door-ack.json"]
    target = devices.get(command["target_device_id"])
    if target is None:
        c.error("door-command.json: target device is not registered")
    elif target["controller_node_id"] != command["target_node_id"]:
        c.error("door-command.json: target node does not control target device")
    if command["action"] not in (target or {}).get("capabilities", []):
        c.error("door-command.json: action is not a registered device capability")
    for key in ("command_id", "trace_id", "target_device_id"):
        if command[key] != ack[key]:
            c.error(f"door command/ACK mismatch: {key}")
    if ack["node_id"] != command["target_node_id"]:
        c.error("door command/ACK mismatch: controller node")
    if command["expires_at_ms"] <= command["issued_at_ms"]:
        c.error("door-command.json: expires_at_ms must be later than issued_at_ms")


def check_sensor_rules(c: Checker, devices: dict[str, Any]) -> None:
    sensor_rules = pfn("sensor-rules.json")
    for reading_name, rule in sensor_rules["readings"].items():
        if "minimum" in rule and "maximum" in rule and rule["minimum"] > rule["maximum"]:
            c.error(f"sensor-rules.json: {reading_name} minimum exceeds maximum")
        if rule["type"] in {"boolean", "string"} and ("minimum" in rule or "maximum" in rule):
            c.error(f"sensor-rules.json: {reading_name} has numeric limits for {rule['type']}")
        for source_id in rule["allowed_source_device_ids"]:
            if source_id not in devices:
                c.error(f"sensor-rules.json: {reading_name} uses unknown source device {source_id}")


def check_scenarios(c: Checker, devices: dict[str, Any]) -> dict[str, Any]:
    scenario_rules = pfn("scenario-rules.json")
    intents = [item["intent"] for item in scenario_rules["scenarios"]]
    if len(intents) != len(set(intents)):
        c.error("scenario-rules.json: duplicate intent")
    if "device_control" in intents:
        c.error("scenario-rules.json: device_control is a core built-in intent")
    for scenario in scenario_rules["scenarios"]:
        for index, rule in enumerate(scenario["commands"]):
            label = f"scenario-rules.json: {scenario['intent']} command {index + 1}"
            target = devices.get(rule["target_device_id"])
            if target is not None and target["controller_node_id"] != rule["target_node_id"]:
                c.error(f"{label}: target node does not control target device")
            parameters = {k: v for k, v in rule["parameters"].items() if k != "auto_off_after_seconds"}
            check_device_action(c, label, target, rule["action"], parameters)
    return {item["intent"]: item for item in scenario_rules["scenarios"]}


def check_protocol(c: Checker, node_ids: set[str]) -> dict[str, Any]:
    protocol = pfn("protocol.json")
    topics = list(protocol["topics"].values())
    if len(topics) != len(set(topics)):
        c.error("protocol.json: duplicate MQTT topic")
    if protocol["offline_after_ms"] < protocol["heartbeat_interval_ms"] * 2:
        c.error("protocol.json: offline threshold is too short for heartbeat interval")
    if protocol["transport_policy"]["retained"] is not False:
        c.error("protocol.json: retained must stay false")
    for message_name, schema_name in protocol.get("message_schemas", {}).items():
        if message_name not in protocol["topics"]:
            c.error(f"protocol.json: schema mapping has no topic named {message_name}")
        if not (PFN / schema_name).is_file():
            c.error(f"protocol.json: missing mapped schema {schema_name}")
    for name, policy in protocol.get("topic_policies", {}).items():
        if name not in protocol["topics"]:
            c.error(f"protocol.json: topic policy for unknown topic {name}")
        for key in ("publishers", "subscribers", "acl_accounts"):
            for node in policy.get(key, []):
                if node not in node_ids:
                    c.error(f"protocol.json: {name} {key} has unknown node {node}")
        if policy.get("contains_raw_transcript") and policy.get("core_subscribes") is not False:
            c.error(f"protocol.json: {name} carries a transcript but core_subscribes is not false")
        if policy.get("contains_secret") and policy.get("core_subscribes") is not False:
            c.error(f"protocol.json: {name} carries a secret but core_subscribes is not false")
        if policy.get("contains_secret") and set(policy.get("acl_accounts", [])) - set(policy.get("subscribers", [])):
            c.error(f"protocol.json: {name} carries a secret; only its subscribers may hold ACL read rights")
    for name in ("ai_llm_request", "ai_llm_result"):
        if protocol.get("topic_policies", {}).get(name, {}).get("core_subscribes") is not False:
            c.error(f"protocol.json: core must not subscribe to {name}")
    schemas = protocol.get("message_schemas", {})
    if "mobile_task_action" in protocol["topics"]:
        if schemas.get("mobile_task_action") != schemas.get("ui_action"):
            c.error("protocol.json: mobile_task_action must carry the ui_action document")
        if mqtt_matches("pfn/v1/ui/#", protocol["topics"]["mobile_task_action"]):
            c.error("protocol.json: mobile_task_action must stay outside pfn/v1/ui/# (the broker tells phone from UI)")
    for intent, spec in protocol.get("task_intents", {}).items():
        if isinstance(spec, dict) and not (PFN / spec["parameters_schema"]).is_file():
            c.error(f"protocol.json: task intent {intent} schema is missing")
    if not (PFN / protocol["ai_action_map"]).is_file():
        c.error("protocol.json: ai_action_map file is missing")
    return protocol


def internal_enum(schema_name: str, prop: str) -> list[Any]:
    schema = load_json(INTERNAL / schema_name)
    return list(schema["properties"][prop]["enum"])


def check_action_map(c: Checker, registry: dict[str, Any], devices: dict[str, Any], scenarios: dict[str, Any]) -> None:
    action_map = pfn("ai-action-map.json")
    c.schema_check("ai-action-map.json", action_map, pfn("ai-action-map.schema.json"))
    zones = set(registry["zones"])
    ai_zones = set(internal_enum("activity-context.schema.json", "presence_zone")) | {"WHOLE_HOME"}
    for ai_zone in sorted(ai_zones - set(action_map["zone_map"])):
        c.error(f"ai-action-map.json: AI zone {ai_zone} has no mapping")
    for ai_zone, zone in action_map["zone_map"].items():
        if zone not in zones:
            c.error(f"ai-action-map.json: zone_map {ai_zone} -> {zone} is not a registry zone")

    ai_devices = {d for d in internal_enum("task-proposal.schema.json", "device") if d is not None}
    ai_actions = {a for a in internal_enum("task-proposal.schema.json", "action") if a is not None}
    for name in sorted(ai_devices - set(action_map["devices"])):
        c.error(f"ai-action-map.json: AI device {name} has no device entry")
    for name, entry in action_map["devices"].items():
        label = f"ai-action-map.json: device {name}"
        if name not in ai_devices:
            c.error(f"{label}: not in task-proposal device vocabulary")
        for key in ("device_id", "number_device_id"):
            device_id = entry.get(key)
            if device_id is not None and device_id not in devices:
                c.error(f"{label}: {key} {device_id} is not registered")
        if entry["enabled"] and entry["device_id"] is not None and devices.get(entry["device_id"], {}).get("wiring_status") != "wired":
            c.error(f"{label}: enabled but {entry['device_id']} is not wired")

    seen: set[tuple[str, str]] = set()
    for index, entry in enumerate(action_map["actions"]):
        key = (entry["ai_device"], entry["ai_action"])
        label = f"ai-action-map.json: actions[{index}] {key[0]}/{key[1]}"
        if key in seen:
            c.error(f"{label}: duplicate mapping")
        seen.add(key)
        if entry["ai_device"] != "*" and entry["ai_device"] not in action_map["devices"]:
            c.error(f"{label}: unknown AI device")
        if entry["ai_action"] not in ai_actions:
            c.error(f"{label}: not in task-proposal action vocabulary")
        if not entry["enabled"] and "pending_decision" not in entry and entry["kind"] != "unsupported":
            c.error(f"{label}: disabled mapping needs pending_decision")
        if entry["kind"] == "scenario":
            if entry.get("intent") not in scenarios:
                c.error(f"{label}: scenario intent {entry.get('intent')} is not in scenario-rules.json")
            continue
        if entry["kind"] != "device_control" or not entry["enabled"]:
            continue
        device_entry = action_map["devices"][entry["ai_device"]]
        if not device_entry["enabled"]:
            c.error(f"{label}: enabled action on a disabled AI device")
        device_id = device_entry.get(entry.get("target", "device_id"))
        device = devices.get(device_id) if device_id else None
        if "command" not in entry:
            c.error(f"{label}: device_control mapping needs command")
            continue
        parameters = dict(entry.get("parameters", {}))
        if "position_ref" in entry:
            if device is None or "positions" not in device:
                c.error(f"{label}: position_ref without registry positions")
                continue
            parameters["position_degrees"] = device["positions"][entry["position_ref"]]
        if "parameter_from" in entry:
            c.error(f"{label}: parameter_from mappings must stay disabled until checked by hand")
        check_device_action(c, label, device, entry["command"], parameters)


def check_task_requests(
    c: Checker, devices: dict[str, Any], scenarios: dict[str, Any], examples: dict[str, Any]
) -> None:
    control_schema = pfn("device-control.schema.json")
    origin_node = {"voice_parser": "NODE-RPI5-02", "llm": "NODE-RPI5-01"}
    for name, doc in examples.items():
        if not name.startswith("task-request-"):
            continue
        label = f"examples/{name}"
        if doc["intent"] == "device_control":
            if not c.schema_check(f"{label} parameters", doc["parameters"], control_schema):
                continue
            control = doc["parameters"]
            if doc["target_device"] != control["target_device_id"]:
                c.error(f"{label}: target_device must equal parameters.target_device_id")
            expected = origin_node.get(control["origin"])
            if expected is not None and doc["source_node_id"] != expected:
                c.error(f"{label}: origin {control['origin']} must come from {expected}")
            check_device_action(c, label, devices.get(control["target_device_id"]), control["action"], control["parameters"])
        elif doc["intent"] not in scenarios:
            c.error(f"{label}: intent {doc['intent']} is neither device_control nor a scenario")


def check_llm_and_perception(
    c: Checker, protocol: dict[str, Any], registry: dict[str, Any], devices: dict[str, Any], examples: dict[str, Any]
) -> None:
    policies = protocol["topic_policies"]
    request = examples["llm-request.json"]
    result = examples["llm-result-task.json"]
    task = examples["task-request-device-control.json"]
    if request["source_node_id"] not in policies["ai_llm_request"]["publishers"]:
        c.error("llm-request.json: publisher is not allowed by topic policy")
    if request["expires_at_ms"] <= request["requested_at_ms"]:
        c.error("llm-request.json: expires_at_ms must be later than requested_at_ms")
    for name in ("llm-result-task.json", "llm-result-clarify.json"):
        if examples[name]["source_node_id"] not in policies["ai_llm_result"]["publishers"]:
            c.error(f"{name}: publisher is not allowed by topic policy")
    for key in ("request_id", "trace_id"):
        if request[key] != result[key]:
            c.error(f"llm request/result mismatch: {key}")
    if result["task_id"] != task["task_id"] or result["trace_id"] != task["trace_id"]:
        c.error("llm-result-task.json: task_id/trace_id do not match the device_control task request")
    if task["parameters"]["ai_device"] != result["ai_device"] or task["parameters"]["ai_action"] != result["ai_action"]:
        c.error("llm-result-task.json: AI device/action differ from the task request")

    listen = examples.get("voice-listen-start.json")
    if listen is not None:
        if listen["source_node_id"] not in policies["ai_voice_listen"]["publishers"]:
            c.error("voice-listen-start.json: publisher is not allowed by topic policy")
        if listen["target_node_id"] not in policies["ai_voice_listen"]["subscribers"]:
            c.error("voice-listen-start.json: target is not a subscriber")
        if listen["expires_at_ms"] <= listen["sent_at_ms"]:
            c.error("voice-listen-start.json: expires_at_ms must be later than sent_at_ms")
    if any(mqtt_matches(pattern, protocol["topics"]["ai_voice_listen"]) for pattern in protocol["core_subscriptions"]):
        c.error("protocol.json: core must not subscribe to ai_voice_listen")

    pair_request = examples.get("mobile-pairing-request.json")
    pair_offer = examples.get("mobile-pairing-offer.json")
    if pair_request is not None and pair_offer is not None:
        for doc, topic, role in ((pair_request, "mobile_pairing_request", "publishers"),
                                 (pair_offer, "mobile_pairing_offer", "publishers")):
            if doc["source_node_id"] not in policies[topic][role]:
                c.error(f"{topic} example: publisher is not allowed by topic policy")
            if doc["target_node_id"] not in policies[topic]["subscribers"]:
                c.error(f"{topic} example: target is not a subscriber")
        if pair_request["expires_at_ms"] <= pair_request["sent_at_ms"]:
            c.error("mobile-pairing-request.json: expires_at_ms must be later than sent_at_ms")
        if pair_offer["request_id"] != pair_request["request_id"]:
            c.error("mobile-pairing-offer.json: request_id does not answer mobile-pairing-request.json")
        if pair_offer["status"] == "offered":
            try:
                qr = json.loads(pair_offer["qr_payload"])
            except json.JSONDecodeError:
                qr = None
                c.error("mobile-pairing-offer.json: qr_payload is not JSON")
            if qr is not None:
                for key, field in (("pairing_code", "pairing_code"), ("base_url", "base_url"),
                                   ("device_name", "device_name")):
                    if qr.get(key) != pair_offer[field]:
                        c.error(f"mobile-pairing-offer.json: qr_payload {key} differs from {field}")
                if qr.get("device_id") != pair_offer["source_node_id"]:
                    c.error("mobile-pairing-offer.json: qr_payload device_id is not the offering node")
            if pair_offer["expires_at_ms"] <= pair_offer["created_at_ms"]:
                c.error("mobile-pairing-offer.json: expires_at_ms must be later than created_at_ms")

    phones_request = examples.get("mobile-phones-request.json")
    phones_state = examples.get("mobile-phones-state.json")
    if phones_request is not None and phones_state is not None:
        for doc, topic in ((phones_request, "mobile_phones_request"), (phones_state, "mobile_phones_state")):
            if doc["source_node_id"] not in policies[topic]["publishers"]:
                c.error(f"{topic} example: publisher is not allowed by topic policy")
            if doc["target_node_id"] not in policies[topic]["subscribers"]:
                c.error(f"{topic} example: target is not a subscriber")
        if phones_request["expires_at_ms"] <= phones_request["sent_at_ms"]:
            c.error("mobile-phones-request.json: expires_at_ms must be later than sent_at_ms")
        if phones_state["request_id"] != phones_request["request_id"]:
            c.error("mobile-phones-state.json: request_id does not answer mobile-phones-request.json")
        if phones_state["action"] != phones_request["action"]:
            c.error("mobile-phones-state.json: action differs from the request it answers")
        if phones_request["action"] == "revoke" and phones_state.get("revoked") and any(
                phone["phone_id"] == phones_request["phone_id"] for phone in phones_state["phones"]):
            c.error("mobile-phones-state.json: the unpaired phone is still listed")
    for name in ("mobile_phones_request", "mobile_phones_state"):
        if name in protocol["topics"] and any(
                mqtt_matches(pattern, protocol["topics"][name]) for pattern in protocol["core_subscriptions"]):
            c.error(f"protocol.json: core must not subscribe to {name}")

    zones = set(registry["zones"])
    for name, doc in examples.items():
        if not name.startswith("perception-"):
            continue
        label = f"examples/{name}"
        if doc["source_node_id"] not in policies["ai_perception"]["publishers"]:
            c.error(f"{label}: publisher is not allowed by topic policy")
        if doc["zone"] not in zones:
            c.error(f"{label}: zone {doc['zone']} is not a registry zone")
        source_device = doc.get("source_device_id")
        if source_device is not None:
            device = devices.get(source_device)
            if device is None or device["controller_node_id"] != doc["source_node_id"]:
                c.error(f"{label}: {source_device} is not attached to {doc['source_node_id']}")
        for candidate in doc["attributes"].get("candidate_devices", []):
            if candidate not in devices:
                c.error(f"{label}: candidate device {candidate} is not registered")


def mqtt_matches(pattern: str, topic: str) -> bool:
    pattern_parts = pattern.split("/")
    topic_parts = topic.split("/")
    for index, part in enumerate(pattern_parts):
        if part == "#":
            return True
        if index >= len(topic_parts):
            return False
        if part != "+" and part != topic_parts[index]:
            return False
    return len(pattern_parts) == len(topic_parts)


def position_ref(device: dict[str, Any] | None, parameters: Any) -> str | None:
    if device is None or "positions" not in device or not isinstance(parameters, dict):
        return None
    degrees = parameters.get("position_degrees")
    for name in ("open", "closed"):
        if degrees == device["positions"][name]:
            return name
    return None


def matching_rule(policy: dict[str, Any], intent: str, origin: str, device_id: str | None,
                  ref: str | None) -> dict[str, Any] | None:
    """Python mirror of core TaskPolicy::decide (first matching rule wins)."""
    for rule in policy["rules"]:
        if "intents" in rule and intent not in rule["intents"]:
            continue
        if "origins" in rule and origin not in rule["origins"]:
            continue
        if "target_device_ids" in rule and device_id not in rule["target_device_ids"]:
            continue
        if "position_refs" in rule and ref not in rule["position_refs"]:
            continue
        return rule
    return None


def decide_approval(policy: dict[str, Any], intent: str, origin: str, device_id: str | None, ref: str | None) -> str:
    rule = matching_rule(policy, intent, origin, device_id, ref)
    return policy["default_approval"] if rule is None else rule["approval"]


def phone_may_approve(policy: dict[str, Any], intent: str, origin: str, device_id: str | None,
                      ref: str | None) -> bool:
    rule = matching_rule(policy, intent, origin, device_id, ref)
    return rule is None or rule.get("phone_may_approve", True)


def proposal_ttl_seconds(policy: dict[str, Any], intent: str, origin: str, device_id: str | None,
                         ref: str | None) -> int:
    """Python mirror of core: how long a task of this kind may wait for approval."""
    rule = matching_rule(policy, intent, origin, device_id, ref)
    return (rule or {}).get("proposal_ttl_seconds", policy["proposal_ttl_seconds"])


# User decision 2026-10-21. (intent, origin, device, position_ref) -> approval
EXPECTED_APPROVALS = [
    ("device_control", "voice_parser", "LED-LIVING-01", None, "auto"),
    ("device_control", "ui", "SERVO-WINDOW-01", "closed", "auto"),
    ("device_control", "voice_parser", "SERVO-DOOR-01", "open", "ui_approval"),
    ("device_control", "ui", "SERVO-DOOR-01", "open", "ui_approval"),
    ("device_control", "voice_parser", "SERVO-DOOR-01", "closed", "auto"),
    ("device_control", "voice_parser", "SERVO-WINDOW-01", "open", "auto"),
    ("device_control", "llm", "LED-LIVING-01", None, "ui_approval"),
    ("device_control", "unspecified", "LED-LIVING-01", None, "ui_approval"),
    ("quiet_mode_on", "context_rule", None, None, "auto"),
    ("quiet_mode_off", "context_rule", None, None, "auto"),
    ("quiet_mode_on", "voice_parser", None, None, "auto"),
    ("quiet_mode_on", "llm", None, None, "ui_approval"),
    ("prepare_home", "unspecified", None, None, "ui_approval"),
    ("prepare_sleep", "voice_parser", None, None, "ui_approval"),
    ("night_bed_exit", "ui", None, None, "ui_approval"),
]

# Decision 2026-10-22: the phone approves what waits, except opening the entrance door.
EXPECTED_PHONE_APPROVALS = [
    ("device_control", "voice_parser", "SERVO-DOOR-01", "open", False),
    ("device_control", "ui", "SERVO-DOOR-01", "open", False),
    ("device_control", "llm", "SERVO-DOOR-01", "open", False),
    ("device_control", "unspecified", "SERVO-DOOR-01", "open", False),
    ("device_control", "llm", "LED-LIVING-01", None, True),
    ("device_control", "unspecified", "LED-LIVING-01", None, True),
    ("quiet_mode_on", "llm", None, None, True),
    ("prepare_home", "unspecified", None, None, True),
    ("prepare_sleep", "voice_parser", None, None, True),
]

# User decision 2026-10-24: waiting tasks expire; opening the entrance door waits only 2 minutes.
EXPECTED_PROPOSAL_TTLS = [
    ("device_control", "voice_parser", "SERVO-DOOR-01", "open", 120),
    ("device_control", "ui", "SERVO-DOOR-01", "open", 120),
    ("device_control", "llm", "SERVO-DOOR-01", "open", 120),
    ("device_control", "llm", "LED-LIVING-01", None, 600),
    ("device_control", "unspecified", "LED-LIVING-01", None, 600),
    ("quiet_mode_on", "llm", None, None, 600),
    ("prepare_home", "unspecified", None, None, 600),
]


def check_policy(c: Checker, registry: dict[str, Any], devices: dict[str, Any], scenarios: dict[str, Any]) -> dict[str, Any]:
    policy = pfn("task-policy.json")
    c.schema_check("task-policy.json", policy, pfn("task-policy.schema.json"))
    node_ids = {item["node_id"] for item in registry["nodes"]}
    control_origins = set(pfn("device-control.schema.json")["properties"]["origin"]["enum"])
    for name in sorted((control_origins | {"context_rule"}) - set(policy["origins"])):
        c.error(f"task-policy.json: origin {name} has no entry")
    for name, entry in policy["origins"].items():
        for node in entry["allowed_source_nodes"]:
            if node not in node_ids:
                c.error(f"task-policy.json: origin {name} allows unknown node {node}")
    known_intents = set(scenarios) | {"device_control"}
    names = [rule["name"] for rule in policy["rules"]]
    if len(names) != len(set(names)):
        c.error("task-policy.json: duplicate rule name")
    for rule in policy["rules"]:
        label = f"task-policy.json: rule {rule['name']}"
        for intent in rule.get("intents", []):
            if intent not in known_intents:
                c.error(f"{label}: unknown intent {intent}")
        for origin in rule.get("origins", []):
            if origin not in policy["origins"]:
                c.error(f"{label}: unknown origin {origin}")
        for device_id in rule.get("target_device_ids", []):
            if device_id not in devices:
                c.error(f"{label}: unknown device {device_id}")
            elif "position_refs" in rule and "positions" not in devices[device_id]:
                c.error(f"{label}: position_refs on {device_id} without registry positions")
        if "phone_may_approve" in rule and rule["approval"] != "ui_approval":
            c.error(f"{label}: phone_may_approve only applies to a rule that waits for approval")
        if "proposal_ttl_seconds" in rule and rule["approval"] != "ui_approval":
            c.error(f"{label}: proposal_ttl_seconds only applies to a rule that waits for approval")
    for intent, origin, device_id, ref, expected in EXPECTED_APPROVALS:
        actual = decide_approval(policy, intent, origin, device_id, ref)
        if actual != expected:
            c.error(f"task-policy.json: {intent}/{origin}/{device_id}/{ref} -> {actual}, decided {expected}")
    for intent, origin, device_id, ref, expected_phone in EXPECTED_PHONE_APPROVALS:
        if phone_may_approve(policy, intent, origin, device_id, ref) != expected_phone:
            c.error(f"task-policy.json: phone approval of {intent}/{origin}/{device_id}/{ref} must be "
                    f"{'allowed' if expected_phone else 'refused'}")
    for intent, origin, device_id, ref, expected_ttl in EXPECTED_PROPOSAL_TTLS:
        actual_ttl = proposal_ttl_seconds(policy, intent, origin, device_id, ref)
        if actual_ttl != expected_ttl:
            c.error(f"task-policy.json: {intent}/{origin}/{device_id}/{ref} waits {actual_ttl} s for approval, "
                    f"decided {expected_ttl} s")
    if "PROPOSAL_EXPIRED" not in policy["rejection_codes"]:
        c.error("task-policy.json: rejection code PROPOSAL_EXPIRED is missing")
    return policy


def check_context_rules(c: Checker, scenarios: dict[str, Any]) -> None:
    rules = pfn("context-rules.json")
    c.schema_check("context-rules.json", rules, pfn("context-rules.schema.json"))
    perception = pfn("perception.schema.json")
    attributes: dict[str, dict[str, Any]] = {}
    for branch in perception["allOf"]:
        kind = branch["if"]["properties"]["observation_type"]["const"]
        attributes[kind] = branch["then"]["properties"]["attributes"]["properties"]
    names = [rule["name"] for rule in rules["rules"]]
    if len(names) != len(set(names)):
        c.error("context-rules.json: duplicate rule name")
    for rule in rules["rules"]:
        label = f"context-rules.json: rule {rule['name']}"
        if rule["intent"] not in scenarios:
            c.error(f"{label}: intent {rule['intent']} is not a scenario")
        known = attributes.get(rule["observation_type"], {})
        for key, value in rule["match"].items():
            if key not in known:
                c.error(f"{label}: {key} is not an attribute of {rule['observation_type']}")
            elif validate(value, known[key]):
                c.error(f"{label}: match value {value!r} is not a valid {key}")
    by_name = {rule["name"]: rule for rule in rules["rules"]}
    for override in rules.get("feedback_overrides", []):
        label = f"context-rules.json: feedback override {override['name']}"
        if override["name"] in by_name:
            c.error(f"{label}: name collides with a rule")
        group = [rule for rule in rules["rules"] if rule["group"] == override["group"]]
        intents = {rule["intent"] for rule in group}
        if not group:
            c.error(f"{label}: group {override['group']} has no rules")
        for key in ("active_intent", "intent"):
            if override[key] not in intents:
                c.error(f"{label}: {key} {override[key]} is not an intent of group {override['group']}")
        if override["active_intent"] == override["intent"]:
            c.error(f"{label}: intent must undo active_intent")
        until = by_name.get(override["suppress_until_rule"])
        if until is None or until["group"] != override["group"]:
            c.error(f"{label}: suppress_until_rule is not a rule of group {override['group']}")
        elif until["intent"] != override["intent"]:
            c.error(f"{label}: suppress_until_rule must end the context with intent {override['intent']}")


def check_core_subscriptions(c: Checker, protocol: dict[str, Any]) -> None:
    subscriptions = protocol["core_subscriptions"]
    published_by_core = {"task_update", "state_response"}
    for name, template in protocol["topics"].items():
        topic = template.replace("{node_id}", "NODE-X").replace("{client_id}", "client-x")
        covered = any(mqtt_matches(pattern, topic) for pattern in subscriptions)
        if name.startswith("ai_llm_") or protocol.get("topic_policies", {}).get(name, {}).get("core_subscribes") is False:
            if covered:
                c.error(f"protocol.json: core_subscriptions would deliver {template} to core")
        elif name not in published_by_core and not covered:
            c.error(f"protocol.json: core_subscriptions miss {template}")


def check_task_update_examples(c: Checker, policy: dict[str, Any], examples: dict[str, Any]) -> None:
    doc = examples.get("task-update-rejected.json")
    if doc is None:
        return
    task = doc["task"]
    if task["state"] != "rejected" or "failure" not in task:
        c.error("task-update-rejected.json: must show a rejected task with failure")
    elif task["failure"]["code"] not in policy["rejection_codes"]:
        c.error("task-update-rejected.json: failure code is not in task-policy rejection_codes")


def check_internal(c: Checker) -> None:
    for path in sorted(INTERNAL.glob("*.schema.json")):
        schema = load_json(path)
        text = json.dumps(schema)
        if "SOURCE_DIRECTION" in text:
            c.error(f"rpi5-internal/{path.name}: SOURCE_DIRECTION was removed (no microphone direction)")
        props = schema.get("properties", {})
        if "source" in props and "source_node_id" not in props:
            c.error(f"rpi5-internal/{path.name}: event schema needs source_node_id")
    examples = check_examples(c, INTERNAL / "examples", INTERNAL_EXAMPLES, INTERNAL)
    for name, doc in examples.items():
        if isinstance(doc, dict) and "source_node_id" in doc and doc["source_node_id"] not in {"NODE-RPI5-01", "NODE-RPI5-02", "NODE-RPI4-01", "PC-SIMULATION"}:
            c.error(f"rpi5-internal/examples/{name}: unknown source_node_id")


def main() -> int:
    c = Checker()
    try:
        return run_checks(c)
    except Exception as error:  # noqa: BLE001 - a crash must still read as a failure
        c.error(f"validator stopped: {type(error).__name__}: {error}")
        return report(c, {}, {}, {})


def run_checks(c: Checker) -> int:
    check_json_files(c)

    registry = pfn("device-registry.json")
    c.schema_check("device-registry.json", registry, pfn("device-registry.schema.json"))
    c.schema_check("sensor-rules.json", pfn("sensor-rules.json"), pfn("sensor-rules.schema.json"))
    c.schema_check("scenario-rules.json", pfn("scenario-rules.json"), pfn("scenario-rules.schema.json"))
    examples = check_examples(c, PFN / "examples", PFN_EXAMPLES, PFN)

    check_registry(c, registry)
    devices = {item["device_id"]: item for item in registry["devices"]}
    node_ids = {item["node_id"] for item in registry["nodes"]}
    if len(examples) == len(PFN_EXAMPLES):
        check_door_pair(c, devices, examples)
    check_sensor_rules(c, devices)
    scenarios = check_scenarios(c, devices)
    protocol = check_protocol(c, node_ids)
    check_action_map(c, registry, devices, scenarios)
    if len(examples) == len(PFN_EXAMPLES):
        check_task_requests(c, devices, scenarios, examples)
        check_llm_and_perception(c, protocol, registry, devices, examples)
    policy = check_policy(c, registry, devices, scenarios)
    check_context_rules(c, scenarios)
    check_core_subscriptions(c, protocol)
    if len(examples) == len(PFN_EXAMPLES):
        check_task_update_examples(c, policy, examples)
    check_internal(c)
    return report(c, node_ids, devices, scenarios)


def report(c: Checker, node_ids: Any, devices: Any, scenarios: Any) -> int:
    if c.errors:
        print("CONTRACT_VALIDATION_FAILED")
        for error in c.errors:
            print(f"- {error}")
        return 1

    print("CONTRACT_VALIDATION_PASS")
    print(
        f"nodes={len(node_ids)} devices={len(devices)} "
        f"schemas={len(list(PFN.glob('*.schema.json')))}+{len(list(INTERNAL.glob('*.schema.json')))} "
        f"examples={c.checked_examples} scenarios={len(scenarios)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
