"""Mobile API <-> RPI4-01 core over pfn.v1 MQTT (system-architecture 3-5).

Phone -> POST command -> pfn/v1/core/task/request (intent device_control, parameters.origin "ui") -> core checks
registry + task-policy -> device command -> ESP ACK -> pfn/v1/core/task/update. Device state changes only when
core reports the task "completed" (that is after the ESP ACK). No direct device topic is used.
MQTT account: homemind-mobile-api (package/mosquitto.acl). Password only from a file path.
"""

from __future__ import annotations

import json
import logging
import os
import threading
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from .registry import ControlRequest

log = logging.getLogger(__name__)

TASK_REQUEST = "pfn/v1/core/task/request"
TASK_UPDATE = "pfn/v1/core/task/update"
STATE_REQUEST = "pfn/v1/core/state/request"
STATE_RESPONSE = "pfn/v1/core/state/response/{client_id}"
# task-policy.json origins.ui.allowed_source_nodes; where this API runs is decided in P7 ([미정]).
ALLOWED_SOURCE_NODES = ("NODE-RPI4-01", "NODE-RPI5-01")
FINAL_STATES = {"completed", "failed", "rejected", "cancelled"}


def now_ms() -> int:
    return int(time.time() * 1000)


@dataclass
class BridgeConfig:
    enabled: bool = False
    host: str = "127.0.0.1"
    port: int = 1883
    username: str | None = None
    password_file: str | None = None
    client_id: str = "homemind-mobile-api"
    source_node_id: str = "NODE-RPI4-01"

    @classmethod
    def from_env(cls) -> "BridgeConfig":
        if os.getenv("HOMEMIND_MQTT_PASSWORD"):
            raise RuntimeError("HOMEMIND_MQTT_PASSWORD is not accepted; put the password in a file and set "
                               "HOMEMIND_MQTT_PASSWORD_FILE")
        config = cls(
            enabled=os.getenv("HOMEMIND_MQTT_ENABLED", "false").lower() == "true",
            host=os.getenv("HOMEMIND_MQTT_HOST", "127.0.0.1"),
            port=int(os.getenv("HOMEMIND_MQTT_PORT", "1883")),
            username=os.getenv("HOMEMIND_MQTT_USERNAME") or None,
            password_file=os.getenv("HOMEMIND_MQTT_PASSWORD_FILE") or None,
            client_id=os.getenv("HOMEMIND_MQTT_CLIENT_ID", "homemind-mobile-api"),
            source_node_id=os.getenv("HOMEMIND_SOURCE_NODE_ID", "NODE-RPI4-01"),
        )
        if config.source_node_id not in ALLOWED_SOURCE_NODES:
            raise RuntimeError(f"HOMEMIND_SOURCE_NODE_ID must be one of {ALLOWED_SOURCE_NODES} (task-policy origin ui)")
        return config

    def password(self) -> str | None:
        if not self.password_file:
            return None
        value = Path(self.password_file).read_text(encoding="utf-8").splitlines()[0].strip()
        if not value:
            raise RuntimeError(f"MQTT password file is empty: {self.password_file}")
        return value


@dataclass
class TrackedTask:
    task_id: str
    device_id: str
    command: str
    state: str = "requested"
    revision: int = 0
    failure: dict[str, Any] | None = None
    requested_at_ms: int = field(default_factory=now_ms)
    updated_at_ms: int = field(default_factory=now_ms)


def device_control_request(control: ControlRequest, *, source_node_id: str) -> dict[str, Any]:
    return {
        "schema_version": "pfn.v1",
        "event_id": str(uuid.uuid4()),
        "trace_id": str(uuid.uuid4()),
        "task_id": str(uuid.uuid4()),
        "source_node_id": source_node_id,
        "intent": "device_control",
        "target_device": control.device_id,
        "priority": 50,
        # Opening the entrance door waits for approval on the 7-inch UI (task-policy rule).
        "safety_level": "caution" if control.position_ref == "open" and control.device_id == "SERVO-DOOR-01" else "normal",
        "interruptible": True,
        "resume_policy": "never",
        "confidence": 1.0,
        "parameters": {
            "target_device_id": control.device_id,
            "action": control.action,
            "parameters": control.parameters,
            "origin": "ui",
            "ai_device": None,
            "ai_action": None,
        },
        "requested_at_ms": now_ms(),
    }


class CoreBridge:
    """Publishes task requests and folds task updates / state responses into the store callbacks."""

    def __init__(self, config: BridgeConfig, *, on_task: Callable[[TrackedTask], None],
                 on_nodes: Callable[[dict[str, str]], None], client_factory: Callable[[str], Any] | None = None) -> None:
        self.config = config
        self.enabled = config.enabled
        self.connected = False
        self._on_task = on_task  # every state change of a tracked task
        self._on_nodes = on_nodes
        self._client_factory = client_factory
        self._client = None
        self._lock = threading.Lock()
        self._tasks: dict[str, TrackedTask] = {}

    # --- lifecycle -------------------------------------------------------------------------------------------
    def start(self) -> None:
        if not self.enabled:
            log.info("MQTT disabled: commands are refused (503)")
            return
        if self._client_factory:
            client = self._client_factory(self.config.client_id)
        else:
            import paho.mqtt.client as mqtt

            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=self.config.client_id)
        if self.config.username:
            client.username_pw_set(self.config.username, self.config.password())
        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        client.reconnect_delay_set(1, 30)
        client.connect_async(self.config.host, self.config.port, keepalive=30)
        client.loop_start()
        self._client = client

    def stop(self) -> None:
        if self._client is not None:
            self._client.disconnect()
            self._client.loop_stop()
            self._client = None
        self.connected = False

    # --- commands --------------------------------------------------------------------------------------------
    def request(self, control: ControlRequest, command: str) -> TrackedTask | None:
        if not self.connected or self._client is None:
            return None
        document = device_control_request(control, source_node_id=self.config.source_node_id)
        task = TrackedTask(task_id=document["task_id"], device_id=control.device_id, command=command)
        with self._lock:
            self._tasks[task.task_id] = task
            self._prune()
        info = self._client.publish(TASK_REQUEST, json.dumps(document, ensure_ascii=False, separators=(",", ":")),
                                    qos=1)
        if getattr(info, "rc", 0) != 0:
            with self._lock:
                self._tasks.pop(task.task_id, None)
            return None
        return task

    def task(self, task_id: str) -> TrackedTask | None:
        with self._lock:
            return self._tasks.get(task_id)

    def pending_for(self, device_id: str) -> TrackedTask | None:
        with self._lock:
            pending = [t for t in self._tasks.values() if t.device_id == device_id and t.state not in FINAL_STATES]
            return max(pending, key=lambda t: t.requested_at_ms) if pending else None

    def last_for(self, device_id: str) -> TrackedTask | None:
        with self._lock:
            tasks = [t for t in self._tasks.values() if t.device_id == device_id]
            return max(tasks, key=lambda t: t.updated_at_ms) if tasks else None

    def request_snapshot(self) -> None:
        if not self.connected or self._client is None:
            return
        self._client.publish(STATE_REQUEST, json.dumps({
            "schema_version": "pfn.v1", "event_id": str(uuid.uuid4()), "trace_id": str(uuid.uuid4()),
            "request_id": str(uuid.uuid4()), "client_id": self.config.client_id, "sections": ["devices", "tasks"],
            "sent_at_ms": now_ms()}), qos=1)

    def _prune(self) -> None:
        cutoff = now_ms() - 10 * 60 * 1000
        for task_id in [t.task_id for t in self._tasks.values() if t.updated_at_ms < cutoff and t.state in FINAL_STATES]:
            del self._tasks[task_id]

    # --- MQTT callbacks --------------------------------------------------------------------------------------
    def _on_connect(self, client, userdata, flags, reason_code, properties=None) -> None:
        del userdata, flags, properties
        if reason_code != 0:
            log.error("MQTT connect failed: %s", reason_code)
            return
        client.subscribe(TASK_UPDATE, qos=1)
        client.subscribe(STATE_RESPONSE.format(client_id=self.config.client_id), qos=1)
        self.connected = True
        self.request_snapshot()

    def _on_disconnect(self, client, userdata, flags, reason_code, properties=None) -> None:
        del client, userdata, flags, reason_code, properties
        self.connected = False

    def _on_message(self, client, userdata, message) -> None:
        del client, userdata
        try:
            document = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            log.warning("invalid JSON on %s", message.topic)
            return
        if message.topic == TASK_UPDATE:
            self.handle_task_update(document)
        elif message.topic.startswith("pfn/v1/core/state/response/"):
            self.handle_state_response(document)

    def handle_task_update(self, document: dict[str, Any]) -> None:
        task = document.get("task") or {}
        with self._lock:
            tracked = self._tasks.get(str(task.get("task_id")))
            if tracked is None or int(task.get("revision", 0)) < tracked.revision:
                return
            tracked.state = str(task.get("state", tracked.state))
            tracked.revision = int(task.get("revision", tracked.revision))
            tracked.failure = task.get("failure")
            tracked.updated_at_ms = now_ms()
        self._on_task(tracked)

    def handle_state_response(self, document: dict[str, Any]) -> None:
        self._on_nodes({str(item.get("node_id")): str(item.get("status")) for item in document.get("devices", [])})
