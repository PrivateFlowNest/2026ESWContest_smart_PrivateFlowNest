from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field


class ComputeNode(BaseModel):
    symbol: str
    id: str
    hardware: str
    role: str
    online: bool = False


class HardwareController(BaseModel):
    name: str
    id: str
    hardware: str
    role: str
    online: bool = False


class HardwareDevice(BaseModel):
    id: str
    name: str
    hardware: str
    controller_id: str | None = None
    controller_name: str
    alias: str | None = None
    category: Literal["actuator", "indicator", "input", "motion", "sensor"]
    wiring_status: str = "wired"
    zone: str | None = None
    online: bool = False
    # Last state confirmed by core (task completed = ESP ACK). Never changed optimistically.
    state: str = "UNKNOWN"
    supported_commands: list[str]
    pending_command: str | None = None
    pending_task_id: str | None = None
    last_task_state: str | None = None
    last_failure_code: str | None = None


class HardwareSnapshot(BaseModel):
    connected: bool
    devices: list[HardwareDevice]
    updated_at: str


class DashboardResponse(BaseModel):
    snapshot: HardwareSnapshot


class HardwareMapResponse(BaseModel):
    nodes: list[ComputeNode]
    controllers: list[HardwareController]
    devices: list[HardwareDevice]


class DeviceCommandRequest(BaseModel):
    command: str


class DeviceCommandResponse(BaseModel):
    device: HardwareDevice
    accepted: bool
    published: bool
    message: str
    task_id: str | None = None
    task_state: str | None = None


class TaskStatusResponse(BaseModel):
    task_id: str
    device_id: str
    command: str
    state: str
    failure_code: str | None = None
    failure_message: str | None = None


class HealthResponse(BaseModel):
    status: Literal["ok"] = "ok"
    service: str = "homemind-mobile-api"
    version: str = "0.4.0-beta"
    mqtt_enabled: bool
    mqtt_connected: bool
    command_path: str = "pfn/v1/core/task/request (core)"


class PairingClaimRequest(BaseModel):
    """QR pairing claim (app: lib/models/pairing_models.dart). code and nonce come from the device's QR."""

    code: str = Field(max_length=32)
    nonce: str = Field(max_length=128)
    device_name: str = Field(default="", max_length=64)


class PairingClaimResponse(BaseModel):
    token: str
    token_id: str
    token_type: Literal["Bearer"] = "Bearer"
    device_id: str
    device_name: str
