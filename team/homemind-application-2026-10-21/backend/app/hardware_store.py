from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
from threading import RLock

from .hardware_models import ComputeNode, HardwareController, HardwareDevice, HardwareSnapshot
from .registry import Registry

_STATE_BY_COMMAND = {"OPEN": "OPEN", "CLOSE": "CLOSED", "ON": "ON", "OFF": "OFF"}


class HardwareStore:
    """App view of registry v3 devices. State changes only through confirm_command (core task completed)."""

    def __init__(self, registry: Registry | None = None) -> None:
        self._lock = RLock()
        self.registry = registry or Registry()
        self._nodes, self._controllers = self.registry.nodes()
        self._devices = {device.id: device for device in self.registry.devices()}

    def snapshot(self, *, connected: bool) -> HardwareSnapshot:
        with self._lock:
            return HardwareSnapshot(
                connected=connected,
                devices=[deepcopy(device) for device in self._devices.values()],
                updated_at=datetime.now(timezone.utc).isoformat(),
            )

    def hardware_map(self) -> tuple[list[ComputeNode], list[HardwareController], list[HardwareDevice]]:
        with self._lock:
            return deepcopy(self._nodes), deepcopy(self._controllers), self.devices()

    def devices(self) -> list[HardwareDevice]:
        with self._lock:
            return [deepcopy(device) for device in self._devices.values()]

    def device(self, device_id: str) -> HardwareDevice | None:
        with self._lock:
            device = self._devices.get(device_id)
            return deepcopy(device) if device else None

    def mark_pending(self, device_id: str, command: str, task_id: str) -> HardwareDevice:
        with self._lock:
            updated = self._devices[device_id].model_copy(update={
                "pending_command": command, "pending_task_id": task_id, "last_task_state": "requested",
                "last_failure_code": None})
            self._devices[device_id] = updated
            return deepcopy(updated)

    def task_changed(self, device_id: str, task_id: str, state: str, failure_code: str | None) -> None:
        with self._lock:
            current = self._devices.get(device_id)
            if current is None or current.pending_task_id != task_id:
                return
            final = state in {"completed", "failed", "rejected", "cancelled"}
            update = {"last_task_state": state, "last_failure_code": failure_code}
            if final:
                update.update(pending_command=None, pending_task_id=None)
            self._devices[device_id] = current.model_copy(update=update)

    def confirm_command(self, device_id: str, command: str) -> None:
        """Core reported the task completed (after the ESP ACK)."""

        with self._lock:
            current = self._devices.get(device_id)
            if current is not None:
                self._devices[device_id] = current.model_copy(
                    update={"state": _STATE_BY_COMMAND[command], "online": True})

    def set_node_status(self, statuses: dict[str, str]) -> None:
        with self._lock:
            for group in (self._nodes, self._controllers):
                for index, item in enumerate(group):
                    if item.id in statuses:
                        group[index] = item.model_copy(update={"online": statuses[item.id] == "online"})
            for device_id, device in self._devices.items():
                if device.controller_id in statuses:
                    self._devices[device_id] = device.model_copy(
                        update={"online": statuses[device.controller_id] == "online"})
