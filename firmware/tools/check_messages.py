#!/usr/bin/env python3
"""Check the firmware decision logic against the pfn.v1 contracts (PC simulation, not hardware).

1. Builds the native CLI (pio run -e native) unless --no-build.
2. Drives it with scenario-rules.json commands, the contract example command and negative cases.
3. Validates every ACK / heartbeat / Last Will / telemetry with the repo validator
   (contracts/validate-contracts.py validate()) and sensor-rules.json, and checks expected
   statuses, duplicate handling, QoS/retained, topics and boot output order.
4. Compares the firmware board config and protocol constants with device-registry.json and
   protocol.json.

Run from anywhere:  python firmware/tools/check_messages.py
Prints FIRMWARE_MESSAGES_PASS or FIRMWARE_MESSAGES_FAIL and exits 0/1.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import runpy
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

FIRMWARE = Path(__file__).resolve().parents[1]
REPO = FIRMWARE.parent
CONTRACTS = REPO / "contracts"
PFN = CONTRACTS / "pfn.v1"
OUT = FIRMWARE / "tools" / "out"

ACT = "CTRL-ESP32-ACT-01"
LED = "CTRL-ESP8266-LED-01"
ISSUED = 1788278400000
TRACE = "22222222-2222-4222-8222-222222222222"


def load(name: str) -> Any:
    with (PFN / name).open(encoding="utf-8") as handle:
        return json.load(handle)


VALIDATOR = runpy.run_path(str(CONTRACTS / "validate-contracts.py"), run_name="validate_contracts")
validate = VALIDATOR["validate"]
SCHEMAS = {
    "command": VALIDATOR["load_schema"](PFN / "command.schema.json"),
    "ack": VALIDATOR["load_schema"](PFN / "ack.schema.json"),
    "heartbeat": VALIDATOR["load_schema"](PFN / "heartbeat.schema.json"),
    "telemetry": VALIDATOR["load_schema"](PFN / "telemetry.schema.json"),
}
REGISTRY = load("device-registry.json")
PROTOCOL = load("protocol.json")
SENSOR_RULES = load("sensor-rules.json")
SCENARIOS = load("scenario-rules.json")
DEVICES = {item["device_id"]: item for item in REGISTRY["devices"]}
NODES = {item["node_id"]: item for item in REGISTRY["nodes"]}

errors: list[str] = []


def error(message: str) -> None:
    errors.append(message)


# ------------------------------------------------------------------------------------- inputs

class Script:
    def __init__(self) -> None:
        self.ops: list[dict[str, Any]] = []
        self.expectations: list[dict[str, Any] | None] = []
        self.counter = 0
        self.elapsed_ms = 0  # simulated time since the current boot (core issues with its clock)

    def op(self, **fields: Any) -> None:
        if fields.get("op") == "boot":
            self.elapsed_ms = 0
        self.elapsed_ms += int(fields.get("advance_ms", 0))
        self.ops.append(fields)
        self.expectations.append(None)

    def command(self, node: str, device: str, action: str, parameters: Any, expect: str,
                code: str | None = None, *, duplicate: bool = False, executed: bool | None = None,
                schema_valid: bool = True, hold_ms: int = 0, reuse: dict[str, Any] | None = None,
                **overrides: Any) -> dict[str, Any]:
        if reuse is not None:
            payload = copy.deepcopy(reuse)
            payload.update(overrides)
        else:
            self.counter += 1
            payload = {
                "schema_version": "pfn.v1",
                "event_id": f"11111111-1111-4111-8111-{self.counter:012d}",
                "trace_id": TRACE,
                "task_id": None,
                "command_id": f"44444444-4444-4444-8444-{self.counter:012d}",
                "target_node_id": node,
                "target_device_id": device,
                "action": action,
                "parameters": parameters,
                "issued_at_ms": ISSUED + self.elapsed_ms,
                "expires_at_ms": ISSUED + self.elapsed_ms + PROTOCOL["command_ttl_ms"],
                "attempt": 1,
            }
            payload.update(overrides)
        problems = validate(payload, SCHEMAS["command"])
        if schema_valid and problems:
            error(f"test input {payload['command_id']} is not a schema-valid command: {problems}")
        if not schema_valid and not problems:
            error(f"test input {payload['command_id']} was meant to be schema-invalid")
        if executed is None:
            executed = expect in ("succeeded", "failed") and not duplicate
        self.ops.append({"op": "command", "payload": payload, "hold_ms": hold_ms})
        self.elapsed_ms += hold_ms
        self.expectations.append({"payload": payload, "status": expect, "code": code,
                                  "duplicate": duplicate, "executed": executed})
        return payload


def build_script() -> Script:
    s = Script()
    s.op(op="board_config")

    # ---- CTRL-ESP32-ACT-01, no wall clock (no SNTP) --------------------------------------
    s.op(op="boot", node=ACT, wall_ms=None)
    s.op(op="heartbeat")
    s.op(op="will")
    s.op(op="telemetry")
    example = json.loads((PFN / "examples" / "door-command.json").read_text(encoding="utf-8"))
    s.ops.append({"op": "command", "payload": example, "hold_ms": 0})
    s.expectations.append({"payload": example, "status": "succeeded", "code": None,
                           "duplicate": False, "executed": True})
    # Every scenario command addressed to this controller.
    for scenario in SCENARIOS["scenarios"]:
        for item in scenario["commands"]:
            if item["target_node_id"] == ACT:
                s.command(ACT, item["target_device_id"], item["action"], item["parameters"], "succeeded")
    door = s.command(ACT, "SERVO-WINDOW-01", "set_position", {"position_degrees": 90}, "succeeded")
    s.command(ACT, "SERVO-DOOR-02", "set_position", {"position_degrees": 90}, "rejected_invalid",
              "DEVICE_NOT_WIRED")  # bathroom door removed from the model (registry v4)
    s.op(op="clock", advance_ms=3000)
    s.command(ACT, "", "", None, "succeeded", duplicate=True, reuse=door, attempt=2)
    s.command(ACT, "", "", None, "succeeded", duplicate=True, reuse=door, attempt=3)
    s.command(ACT, "SERVO-DOOR-01", "set_position", {"position_degrees": 45}, "rejected_invalid",
              "POSITION_NOT_REGISTERED")
    s.command(ACT, "SERVO-DOOR-01", "set_position", {"position_degrees": 200}, "rejected_invalid",
              "PARAMETERS_INVALID", schema_valid=False)
    s.command(ACT, "SERVO-BED-CURTAIN-01", "set_position", {"position_degrees": 0}, "rejected_invalid",
              "DEVICE_NOT_WIRED")
    s.command(ACT, "SERVO-GARAGE-01", "set_position", {"position_degrees": 0}, "rejected_invalid",
              "DEVICE_UNKNOWN")
    s.command(ACT, "SERVO-DOOR-01", "set_switch", {"on": True}, "rejected_invalid", "CAPABILITY_NOT_ALLOWED")
    s.command(ACT, "SERVO-WINDOW-01", "set_position", {"position_degrees": 0}, "rejected_invalid", "NODE_MISMATCH",
              target_node_id=LED)
    s.command(ACT, "SERVO-WINDOW-01", "set_position", {"position_degrees": 0}, "rejected_expired", "COMMAND_EXPIRED",
              hold_ms=PROTOCOL["command_ttl_ms"])
    s.command(ACT, "FAN-01", "set_switch", {"on": True}, "rejected_invalid", "DEVICE_UNKNOWN")  # on the ESP8266 (v4)
    s.command(ACT, "SERVO-WINDOW-01", "set_position", {"position_degrees": 90}, "rejected_expired",
              "COMMAND_EXPIRED", issued_at_ms=ISSUED - 60000, expires_at_ms=ISSUED - 50000)
    s.op(op="command", payload="this is not json", hold_ms=0)
    s.expectations[-1] = {"payload": None, "status": None, "ackable": False}
    s.op(op="clock", advance_ms=2000)
    s.op(op="telemetry")
    s.op(op="heartbeat")

    # ---- CTRL-ESP32-ACT-01 with SNTP wall clock -------------------------------------------
    s.op(op="boot", node=ACT, wall_ms=ISSUED + 200)
    s.op(op="heartbeat")
    s.command(ACT, "SERVO-DOOR-01", "set_position", {"position_degrees": 90}, "succeeded")
    s.command(ACT, "SERVO-DOOR-01", "set_position", {"position_degrees": 0}, "rejected_expired",
              "COMMAND_EXPIRED", issued_at_ms=ISSUED - 10200, expires_at_ms=ISSUED - 200)
    s.op(op="telemetry")

    # ---- CTRL-ESP8266-LED-01 ---------------------------------------------------------------
    s.op(op="boot", node=LED, wall_ms=None)
    s.op(op="heartbeat")
    s.op(op="will")
    s.op(op="telemetry")
    for scenario in SCENARIOS["scenarios"]:
        for item in scenario["commands"]:
            if item["target_node_id"] == LED:
                s.command(LED, item["target_device_id"], item["action"], item["parameters"], "succeeded")
        s.op(op="clock", advance_ms=500)
        s.op(op="telemetry")
    quiet = s.command(LED, "LED-AIRCON-01", "set_color", {"color": "red", "pattern": "blink"}, "succeeded")
    s.command(LED, "", "", None, "succeeded", duplicate=True, reuse=quiet, attempt=2)
    s.op(op="clock", advance_ms=1000)
    s.op(op="telemetry")
    s.command(LED, "LED-LIVING-01", "set_switch", {"on": True, "auto_off_after_seconds": 15}, "succeeded")
    s.op(op="clock", advance_ms=15000)
    s.op(op="telemetry")
    s.command(LED, "LED-AIRCON-01", "set_color", {"color": "off"}, "succeeded")
    s.command(LED, "LED-AIRCON-01", "set_color", {"color": "green"}, "rejected_invalid", "COLOR_NOT_SUPPORTED")
    s.command(LED, "LED-AIRCON-01", "set_color", {"color": "amber", "pattern": "blink"}, "rejected_invalid",
              "COLOR_NOT_SUPPORTED")
    # registry v4: FAN-01 on D2 of this board; TV and washer LEDs are not in the model
    s.command(LED, "FAN-01", "set_switch", {"on": True}, "succeeded")
    s.command(LED, "FAN-01", "set_level", {"level_percent": 30}, "rejected_invalid", "CAPABILITY_NOT_ALLOWED")
    s.op(op="telemetry")
    s.command(LED, "LED-TV-01", "set_color", {"color": "red", "pattern": "blink"}, "rejected_invalid",
              "DEVICE_NOT_WIRED")
    s.command(LED, "LED-GUIDE-01", "set_switch", {"on": True, "auto_off_after_seconds": 15},
              "rejected_invalid", "DEVICE_NOT_WIRED")
    s.command(LED, "FND-WASHER-01", "set_number", {"value": 43}, "rejected_invalid", "DEVICE_NOT_WIRED")
    s.command(LED, "LED-AIRCON-01", "set_level", {"level_percent": 50}, "rejected_invalid", "CAPABILITY_NOT_ALLOWED")
    s.command(LED, "LED-LIVING-01", "set_switch", {"on": "yes"}, "rejected_invalid", "PARAMETERS_INVALID",
              schema_valid=False)
    s.op(op="heartbeat")
    return s


# ------------------------------------------------------------------------------------- run

def find_pio() -> list[str]:
    for candidate in (FIRMWARE / ".venv-pio" / "Scripts" / "pio.exe", FIRMWARE / ".venv-pio" / "bin" / "pio"):
        if candidate.exists():
            return [str(candidate)]
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return [found]
    raise SystemExit("PlatformIO not found: create firmware/.venv-pio (see firmware/README.md)")


def cli_path() -> Path:
    name = "program.exe" if os.name == "nt" else "program"
    return FIRMWARE / ".pio" / "build" / "native" / name


def run_cli(script: Script, build: bool) -> list[dict[str, Any]]:
    environment = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
    if build:
        result = subprocess.run(find_pio() + ["run", "-e", "native", "-d", str(FIRMWARE)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                env=environment)
        if result.returncode != 0:
            print(result.stdout[-3000:], result.stderr[-3000:])
            raise SystemExit("native CLI build failed")
    stdin = "\n".join(json.dumps(item, separators=(",", ":")) for item in script.ops) + "\n"
    result = subprocess.run([str(cli_path())], input=stdin, capture_output=True, text=True,
                            encoding="utf-8")
    if result.returncode != 0:
        raise SystemExit(f"native CLI exited with {result.returncode}: {result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


# ------------------------------------------------------------------------------------- checks

def check_board_config(config: dict[str, Any]) -> None:
    if config["registry_version"] != REGISTRY["registry_version"]:
        error(f"board_config registry_version {config['registry_version']} != registry "
              f"{REGISTRY['registry_version']}")
    protocol = config["protocol"]
    for key in ("heartbeat_interval_ms", "offline_after_ms", "command_ttl_ms", "ack_timeout_ms",
                "maximum_payload_bytes"):
        if protocol[key] != PROTOCOL[key]:
            error(f"firmware {key}={protocol[key]} != protocol.json {PROTOCOL[key]}")
    for key in ("commands_and_user_actions_qos", "telemetry_heartbeat_health_qos", "retained"):
        if protocol[key] != PROTOCOL["transport_policy"][key]:
            error(f"firmware {key} != protocol.json transport_policy")
    for node in config["nodes"]:
        node_id = node["node_id"]
        if node_id not in NODES:
            error(f"board_config node {node_id} is not in the registry")
            continue
        if node["node_type"] != NODES[node_id]["node_type"]:
            error(f"{node_id}: node_type {node['node_type']} != registry {NODES[node_id]['node_type']}")
        wired = {d["device_id"] for d in REGISTRY["devices"]
                 if d["controller_node_id"] == node_id and d["wiring_status"] == "wired"}
        unwired = {d["device_id"] for d in REGISTRY["devices"]
                   if d["controller_node_id"] == node_id and d["wiring_status"] != "wired"}
        firmware_devices = {d["device_id"]: d for d in node["devices"]}
        if set(firmware_devices) != wired:
            error(f"{node_id}: firmware devices {sorted(firmware_devices)} != registry wired {sorted(wired)}")
        if set(node["unwired_device_ids"]) != unwired:
            error(f"{node_id}: firmware unwired {sorted(node['unwired_device_ids'])} != registry {sorted(unwired)}")
        used_gpios = set()
        for device_id, device in firmware_devices.items():
            registry = DEVICES.get(device_id)
            if registry is None:
                continue
            if device["pin"] != registry.get("pin"):
                error(f"{device_id}: pin {device['pin']} != registry {registry.get('pin')}")
            if sorted(device["capabilities"]) != sorted(registry["capabilities"]):
                error(f"{device_id}: capabilities {device['capabilities']} != registry {registry['capabilities']}")
            if device.get("positions") != registry.get("positions"):
                error(f"{device_id}: positions {device.get('positions')} != registry {registry.get('positions')}")
            if device.get("supported_colors") != registry.get("supported_colors"):
                error(f"{device_id}: colours {device.get('supported_colors')} != registry "
                      f"{registry.get('supported_colors')}")
            rule = SENSOR_RULES["readings"].get(device["reading_name"])
            if rule is None or device_id not in rule["allowed_source_device_ids"]:
                error(f"{device_id}: reading {device['reading_name']} not allowed by sensor-rules.json")
            if device["gpio"] in node["forbidden_gpios"]:
                error(f"{device_id}: GPIO{device['gpio']} is a forbidden pin")
            if device["gpio"] in used_gpios:
                error(f"{device_id}: GPIO{device['gpio']} used twice")
            used_gpios.add(device["gpio"])


def check_readings(label: str, node_id: str, payload: dict[str, Any]) -> None:
    readings = payload["readings"]
    sources = payload["reading_sources"]
    if payload["sensor_ready"] and not readings:
        error(f"{label}: sensor_ready telemetry without readings")
    if set(readings) != set(sources):
        error(f"{label}: readings and reading_sources keys differ")
    for name, value in readings.items():
        rule = SENSOR_RULES["readings"].get(name)
        if rule is None:
            error(f"{label}: unknown reading {name}")
            continue
        type_ok = {
            "number": isinstance(value, (int, float)) and not isinstance(value, bool),
            "integer": isinstance(value, int) and not isinstance(value, bool),
            "boolean": isinstance(value, bool),
            "string": isinstance(value, str),
        }[rule["type"]]
        if not type_ok:
            error(f"{label}: {name}={value!r} is not {rule['type']}")
        if "minimum" in rule and isinstance(value, (int, float)) and value < rule["minimum"]:
            error(f"{label}: {name} below minimum")
        if "maximum" in rule and isinstance(value, (int, float)) and value > rule["maximum"]:
            error(f"{label}: {name} above maximum")
        if "enum" in rule and value not in rule["enum"]:
            error(f"{label}: {name}={value!r} not in enum")
        source = sources.get(name)
        if source not in rule["allowed_source_device_ids"]:
            error(f"{label}: {name} source {source} not allowed")
        elif DEVICES[source]["controller_node_id"] != node_id or DEVICES[source]["wiring_status"] != "wired":
            error(f"{label}: {name} source {source} is not a wired device of {node_id}")


def check_message(message: dict[str, Any], counts: dict[str, int]) -> str | None:
    kind = message["kind"]
    payload = message["payload"]
    label = f"{kind}#{counts.get(kind, 0)}"
    counts[kind] = counts.get(kind, 0) + 1
    if payload is None:
        error(f"{label}: message did not fit the buffer or is not JSON")
        return None
    schema = SCHEMAS["heartbeat" if kind == "will" else kind]
    for problem in validate(payload, schema):
        error(f"{label}: {problem}")
    if message["bytes"] > PROTOCOL["maximum_payload_bytes"]:
        error(f"{label}: payload too large")
    if message["retained"] is not PROTOCOL["transport_policy"]["retained"]:
        error(f"{label}: retained must be false")
    expected_qos = (PROTOCOL["transport_policy"]["commands_and_user_actions_qos"] if kind == "ack"
                    else PROTOCOL["transport_policy"]["telemetry_heartbeat_health_qos"])
    if message["qos"] != expected_qos:
        error(f"{label}: qos {message['qos']} != {expected_qos}")
    node_id = payload.get("node_id")
    topics = PROTOCOL["topics"]
    expected_topic = {
        "ack": topics["device_ack"].format(node_id=node_id),
        "telemetry": topics["device_telemetry"].format(node_id=node_id),
        "heartbeat": topics["system_heartbeat"],
        "will": topics["system_heartbeat"],
    }[kind]
    if message["topic"] != expected_topic:
        error(f"{label}: topic {message['topic']} != {expected_topic}")
    if node_id not in NODES:
        error(f"{label}: node_id {node_id} not in registry")
    if kind in ("heartbeat", "will"):
        if payload.get("node_type") != NODES.get(node_id, {}).get("node_type"):
            error(f"{label}: node_type does not match registry")
        if payload.get("status") != ("online" if kind == "heartbeat" else "offline"):
            error(f"{label}: wrong status {payload.get('status')}")
    if kind == "telemetry":
        check_readings(label, node_id, payload)
    return node_id


def group_outputs(script: Script, output: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    groups: list[list[dict[str, Any]]] = [[]]
    for item in output:
        if item.get("kind") == "end":
            groups.append([])
        else:
            groups[-1].append(item)
    if groups[-1]:
        error(f"{len(groups[-1])} CLI output lines after the last end marker")
    groups = groups[:-1]
    if len(groups) != len(script.ops):
        error(f"CLI answered {len(groups)} operations, sent {len(script.ops)}")
    return groups


def check_run(script: Script, output: list[dict[str, Any]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    current_node = None
    event_ids: set[str] = set()
    forbidden: dict[str, set[int]] = {}
    devices_by_node: dict[str, list[dict[str, Any]]] = {}

    def check_published(item: dict[str, Any]) -> str | None:
        node_id = check_message(item, counts)
        if node_id != current_node:
            error(f"{item['kind']}: node_id {node_id} != {current_node}")
        if item["payload"]:
            if item["payload"]["event_id"] in event_ids:
                error(f"{item['kind']}: event_id reused")
            event_ids.add(item["payload"]["event_id"])
        return node_id

    for op, expectation, group in zip(script.ops, script.expectations, group_outputs(script, output)):
        name = op["op"]
        for item in group:
            if item["kind"] == "cli_error":
                error(f"op {name}: CLI error {item['message']}")
        hal_events = [item for item in group if item["kind"] == "hal"]
        if name == "boot":
            current_node = op["node"]
        if current_node is not None:
            for event in hal_events:
                if event["gpio"] in forbidden[current_node]:
                    error(f"{current_node}: drove forbidden GPIO{event['gpio']}")

        if name == "board_config":
            config = [item for item in group if item["kind"] == "board_config"]
            if len(config) != 1:
                error("expected one board_config output")
                continue
            check_board_config(config[0])
            forbidden = {n["node_id"]: set(n["forbidden_gpios"]) for n in config[0]["nodes"]}
            devices_by_node = {n["node_id"]: n["devices"] for n in config[0]["nodes"]}
        elif name == "boot":
            current_node = op["node"]
            devices = devices_by_node[current_node]
            servos = [d for d in devices if "positions" in d]
            for index, servo in enumerate(servos):
                # registry v4 open/close motors: signal held low, no pulses until the first command
                wanted = {"kind": "hal", "op": "servo_low", "gpio": servo["gpio"], "value": 0}
                if index >= len(hal_events) or hal_events[index] != wanted:
                    error(f"{current_node}: boot output {index} does not hold {servo['device_id']} low")
            if any(e["op"] != "digital" or e["value"] != 0 for e in hal_events[len(servos):]):
                error(f"{current_node}: after the servos, boot must drive every switch/LED low")
            if len(hal_events) != len(devices):
                error(f"{current_node}: boot drove {len(hal_events)} outputs, expected {len(devices)}")
            for event in hal_events:
                if event["gpio"] in forbidden[current_node]:
                    error(f"{current_node}: boot touched forbidden GPIO{event['gpio']}")
        elif name == "command":
            results = [item for item in group if item["kind"] == "result"]
            acks = [item for item in group if item["kind"] == "ack"]
            if len(results) != 1 or len(acks) > 1:
                error(f"command op: expected one result and at most one ACK, got {len(results)}/{len(acks)}")
                continue
            result = results[0]
            if expectation is None:
                continue
            if expectation.get("ackable") is False:
                if result["ackable"] or acks or hal_events:
                    error("malformed command must be dropped without ACK or output")
                continue
            command = expectation["payload"]
            label = (f"{command['target_node_id']}/{command['target_device_id']}/{command['action']} "
                     f"{json.dumps(command['parameters'])} attempt={command['attempt']}")
            if not acks:
                error(f"{label}: no ACK")
                continue
            node_id = check_published(acks[0])
            payload = acks[0]["payload"]
            if payload is None:
                continue
            if payload["status"] != expectation["status"]:
                error(f"{label}: status {payload['status']} != expected {expectation['status']}")
            code = (payload.get("error") or {}).get("code")
            if code != expectation["code"]:
                error(f"{label}: error code {code} != expected {expectation['code']}")
            if payload["duplicate"] is not expectation["duplicate"]:
                error(f"{label}: duplicate {payload['duplicate']} != {expectation['duplicate']}")
            if result["executed"] is not expectation["executed"]:
                error(f"{label}: executed {result['executed']} != {expectation['executed']}")
            if not expectation["executed"] and hal_events:
                error(f"{label}: outputs driven although not executed: {hal_events}")
            if payload["ok"] is not (expectation["status"] == "succeeded"):
                error(f"{label}: ok flag inconsistent with status")
            for field in ("command_id", "trace_id", "target_device_id"):
                if payload[field] != command[field]:
                    error(f"{label}: ACK {field} {payload[field]} != command {command[field]}")
            if node_id != current_node:
                error(f"{label}: ACK node_id {node_id} != {current_node}")
        else:
            for item in group:
                if item["kind"] in ("heartbeat", "will", "telemetry"):
                    check_published(item)
            if name == "telemetry":
                published = [item for item in group if item["kind"] == "telemetry"]
                if len(published) != len(devices_by_node[current_node]):
                    error(f"{current_node}: expected one telemetry message per wired device")
    return counts


def check_led_states(output: list[dict[str, Any]]) -> None:
    """Spot checks of the LED rule outcomes in the LED run (telemetry after auto-off etc.)."""
    led_telemetry = [o["payload"] for o in output if o.get("kind") == "telemetry"
                     and o["payload"]["node_id"] == LED]
    def last(device: str, reading: str) -> Any:
        values = [t["readings"][reading] for t in led_telemetry
                  if t["reading_sources"].get(reading) == device]
        return values
    living = last("LED-LIVING-01", "light_on")
    if True not in living or living[-1] is not False:
        error("LED-LIVING-01 must have been on and be off after auto_off_after_seconds=15")
    aircon_patterns = last("LED-AIRCON-01", "light_pattern")  # registry v4: the only quiet-mode LED
    if "blink" not in aircon_patterns:
        error("LED-AIRCON-01 never reported light_pattern blink after quiet_mode_on")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-build", action="store_true", help="use the existing native CLI binary")
    arguments = parser.parse_args()

    script = build_script()
    output = run_cli(script, build=not arguments.no_build)
    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / "samples.jsonl").open("w", encoding="utf-8") as handle:
        for item in output:
            handle.write(json.dumps(item, ensure_ascii=False) + "\n")
    counts = check_run(script, output)
    check_led_states(output)

    if errors:
        for message in errors:
            print(f"ERROR {message}")
        print(f"FIRMWARE_MESSAGES_FAIL errors={len(errors)}")
        return 1
    commands = sum(1 for op in script.ops if op["op"] == "command")
    print(f"commands={commands} acks={counts.get('ack', 0)} heartbeats={counts.get('heartbeat', 0)} "
          f"wills={counts.get('will', 0)} telemetry={counts.get('telemetry', 0)} "
          f"samples={OUT.relative_to(REPO).as_posix()}/samples.jsonl")
    print("FIRMWARE_MESSAGES_PASS (PC simulation of the firmware logic, not a hardware test)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
