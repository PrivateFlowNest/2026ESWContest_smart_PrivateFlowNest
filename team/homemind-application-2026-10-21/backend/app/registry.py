"""Device list for the app, read from contracts/pfn.v1/device-registry.json (registry v4).

App commands (OPEN/CLOSE/ON/OFF) become pfn.v1 device_control parameters the same way the voice parser
and the 7-inch UI build them; core validates them again against the same registry.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .hardware_models import ComputeNode, HardwareController, HardwareDevice

DEFAULT_REGISTRY = Path(__file__).resolve().parents[4] / "contracts" / "pfn.v1" / "device-registry.json"

_NAMES = {
    "entrance_door": "현관문",
    "bathroom_door": "화장실 문",
    "window": "창문",
    "ventilation_fan": "환기팬",
    "washer_indicator": "세탁기 표시등",
    "air_conditioner_indicator": "에어컨 표시등",
    "tv_indicator": "TV 표시등",
    "laptop_indicator": "노트북 표시등",
    "living_light": "거실 조명",
    "night_guide_light": "야간 안내등",
    "bed_curtain": "침대 커튼",
    "washer_sound_display": "세탁기 숫자 표시",
    "air_conditioner_sound_display": "에어컨 숫자 표시",
    "tv_sound_display": "TV 숫자 표시",
    "voice_recognition": "실내 마이크",
    "indoor_camera": "실내 카메라",
    "outdoor_camera": "현관 밖 카메라",
    "xy_axis_x": "X축 이동",
    "xy_axis_y": "Y축 이동",
    "x_axis_home": "X축 원점 센서",
    "y_axis_home": "Y축 원점 센서",
}
_ROLES = {
    "core": "백엔드: MQTT 브로커, core, 규칙·정책",
    "octopus_klipper_host": "Octopus Klipper 호스트(HomeMind와 분리)",
    "ui_and_llm": "7인치 UI와 EXAONE",
    "perception": "인식: 음성·소리 상황·카메라",
    "door_and_window_controller": "현관문·창문 제어",
    "light_and_fan_controller": "거실 조명·에어컨 표시등·환기팬 제어",
    "xy_motion_controller": "인형 XY 이동",
}
_HARDWARE = {"pi4": "Raspberry Pi 4", "pi5": "Raspberry Pi 5", "esp32": "ESP32", "esp8266": "ESP8266",
             "octopus": "BTT Octopus V1.1"}
# Device hardware shown as is (MG90S, LED, ...) except these registry codes.
_DEVICE_HARDWARE = {"OPEN_CLOSE_SERVO": "열기/닫기 모터"}
# Kept in the registry but not shown in the app (registry v4, 2026-10-25): the doll axis plays the person and is
# not a home device (Octopus host and controller, XY motors, home sensors), and the removed bathroom door.
_HIDDEN_ROLES = {"octopus_klipper_host", "xy_motion_controller"}


@dataclass(frozen=True)
class ControlRequest:
    device_id: str
    action: str  # set_position | set_switch
    parameters: dict[str, Any]
    position_ref: str | None  # open | closed for servos


def _category(device: dict[str, Any]) -> str:
    capabilities = set(device.get("capabilities", []))
    if capabilities & {"set_position"} or device["device_id"].startswith("FAN-"):
        return "actuator"
    if capabilities & {"set_switch", "set_color", "set_number"}:
        return "indicator"
    if capabilities & {"move_axis", "home_axis"}:
        return "motion"
    if "binary_state" in capabilities:
        return "sensor"
    return "input"


def _commands(device: dict[str, Any]) -> list[str]:
    if device.get("wiring_status") != "wired":
        return []
    capabilities = set(device.get("capabilities", []))
    if "set_position" in capabilities and device.get("positions"):
        return ["OPEN", "CLOSE"]
    if "set_switch" in capabilities:
        return ["ON", "OFF"]
    return []


class Registry:
    def __init__(self, path: str | Path | None = None) -> None:
        path = Path(path or os.getenv("HOMEMIND_DEVICE_REGISTRY") or DEFAULT_REGISTRY)
        self.path = path
        self.raw = json.loads(path.read_text(encoding="utf-8"))
        self._devices = {item["device_id"]: item for item in self.raw["devices"]}
        self._nodes = {item["node_id"]: item for item in self.raw["nodes"]}

    def devices(self) -> list[HardwareDevice]:
        out = []
        for device in self.raw["devices"]:
            controller = self._nodes.get(device.get("controller_node_id", ""), {})
            if device.get("assignment_status") == "removed_from_model" or controller.get("role") in _HIDDEN_ROLES:
                continue
            out.append(HardwareDevice(
                id=device["device_id"],
                name=_NAMES.get(device.get("function", ""), device["device_id"]),
                hardware=_DEVICE_HARDWARE.get(device.get("hardware", ""), device.get("hardware", "")),
                controller_id=device.get("controller_node_id"),
                controller_name=_HARDWARE.get(controller.get("node_type", ""), device.get("controller_node_id", "")),
                alias=device.get("channel") or device.get("pin"),
                category=_category(device),
                wiring_status=device.get("wiring_status", "unknown"),
                zone=device.get("zone"),
                supported_commands=_commands(device),
            ))
        return out

    def nodes(self) -> tuple[list[ComputeNode], list[HardwareController]]:
        nodes, controllers = [], []
        for node in self.raw["nodes"]:
            if node.get("role") in _HIDDEN_ROLES:
                continue
            hardware = _HARDWARE.get(node.get("node_type", ""), node.get("node_type", ""))
            role = _ROLES.get(node.get("role", ""), node.get("role", ""))
            if node["node_id"].startswith("NODE-"):
                nodes.append(ComputeNode(symbol=node.get("asset_label", ""), id=node["node_id"], hardware=hardware,
                                         role=role))
            else:
                controllers.append(HardwareController(name=node.get("asset_label", ""), id=node["node_id"],
                                                      hardware=hardware, role=role))
        return nodes, controllers

    def device(self, device_id: str) -> dict[str, Any] | None:
        return self._devices.get(device_id)

    def control(self, device_id: str, command: str) -> ControlRequest:
        """App command -> device_control. Raises KeyError (unknown) or ValueError (not allowed)."""

        device = self._devices[device_id]
        if command not in _commands(device):
            raise ValueError(f"{command} is not supported by {device_id}")
        if command in {"OPEN", "CLOSE"}:
            ref = "open" if command == "OPEN" else "closed"
            return ControlRequest(device_id, "set_position", {"position_degrees": device["positions"][ref]}, ref)
        return ControlRequest(device_id, "set_switch", {"on": command == "ON"}, None)
