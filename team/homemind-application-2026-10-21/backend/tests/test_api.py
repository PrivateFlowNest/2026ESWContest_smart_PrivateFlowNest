from __future__ import annotations

import json
import os
import socket
import time
import uuid
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from jsonschema import Draft202012Validator, FormatChecker

from app import main
from app.auth import AuthConfig, PairingService
from app.core_bridge import TASK_REQUEST, TASK_UPDATE, BridgeConfig, CoreBridge
from app.hardware_store import HardwareStore

CONTRACTS = Path(__file__).resolve().parents[4] / "contracts" / "pfn.v1"


def _validator(name: str) -> Draft202012Validator:
    return Draft202012Validator(json.loads((CONTRACTS / name).read_text(encoding="utf-8")),
                                format_checker=FormatChecker())


class FakeClient:
    """Stands in for paho's Client: connects at once and records publishes."""

    def __init__(self, client_id: str) -> None:
        self.client_id = client_id
        self.published: list[tuple[str, dict]] = []
        self.subscribed: list[str] = []
        self.on_connect = self.on_disconnect = self.on_message = None

    def username_pw_set(self, *_):
        pass

    def reconnect_delay_set(self, *_):
        pass

    def connect_async(self, *_ , **__):
        pass

    def loop_start(self):
        self.on_connect(self, None, None, 0, None)

    def loop_stop(self):
        pass

    def disconnect(self):
        pass

    def subscribe(self, topic, qos=0):
        self.subscribed.append(topic)

    def publish(self, topic, payload, qos=0):
        self.published.append((topic, json.loads(payload)))
        return type("Info", (), {"rc": 0})()


@pytest.fixture(autouse=True)
def token(monkeypatch, tmp_path) -> str:
    """Every test gets its own pairing data dir (temp) and one paired phone; returns its bearer token."""

    service = PairingService(AuthConfig(data_dir=tmp_path / "mobile-api", public_base_url="http://192.0.2.10:8000"))
    service.prepare()
    _, value = service.tokens.issue("pytest phone")
    monkeypatch.setattr(main, "auth", service)
    return value


def _client(token: str) -> TestClient:
    return TestClient(main.app, headers={"Authorization": f"Bearer {token}"})


@pytest.fixture()
def api(monkeypatch, token):
    """App wired to a fresh store and a fake MQTT client."""

    store = HardwareStore()
    fake: dict[str, FakeClient] = {}

    def factory(client_id: str) -> FakeClient:
        fake["client"] = FakeClient(client_id)
        return fake["client"]

    def on_task(task):
        if task.state == "completed":
            store.confirm_command(task.device_id, task.command)
        store.task_changed(task.device_id, task.task_id, task.state, (task.failure or {}).get("code"))

    bridge = CoreBridge(BridgeConfig(enabled=True), on_task=on_task, on_nodes=store.set_node_status,
                        client_factory=factory)
    monkeypatch.setattr(main, "store", store)
    monkeypatch.setattr(main, "core", bridge)
    with _client(token) as client:
        yield client, fake["client"], bridge


def _update(task_id: str, state: str, revision: int, failure: dict | None = None) -> dict:
    task = {"task_id": task_id, "state": state, "revision": revision}
    if failure:
        task["failure"] = failure
    return {"task": task}


def test_health_and_map_use_registry_v4(token) -> None:
    with _client(token) as client:
        health = client.get("/api/v1/health").json()
        devices = {item["id"]: item for item in client.get("/api/v1/hardware-map").json()["devices"]}
        nodes = {item["id"] for item in client.get("/api/v1/hardware-map").json()["nodes"]}
    assert health["status"] == "ok" and health["version"] == "0.4.0-beta"
    assert nodes == {"NODE-RPI4-01", "NODE-RPI5-01", "NODE-RPI5-02"}  # doll axis host hidden
    assert devices["SERVO-DOOR-01"]["hardware"] == "열기/닫기 모터"
    assert not {"SERVO-DOOR-02", "LED-TV-01", "LED-GUIDE-01", "MOTOR-XY-X-01", "SENSOR-HOME-Y-01"} & set(devices)
    assert len(devices) == 8  # 21 registry IDs - 9 not in the model - 4 doll axis IDs
    assert devices["LED-LIVING-01"]["supported_commands"] == ["ON", "OFF"]
    assert devices["FAN-01"]["controller_id"] == "CTRL-ESP8266-LED-01" and devices["FAN-01"]["supported_commands"] == ["ON", "OFF"]
    assert devices["CAM-INDOOR-01"]["supported_commands"] == []


def test_unknown_or_read_only_device_cannot_be_controlled(token) -> None:
    with _client(token) as client:
        assert client.post("/api/v1/devices/HEATING-01/commands", json={"command": "ON"}).status_code == 404
        assert client.post("/api/v1/devices/CAM-INDOOR-01/commands", json={"command": "ON"}).status_code == 400
        # not in the model (registry v4): the app does not list it, so it is unknown here
        assert client.post("/api/v1/devices/LED-GUIDE-01/commands", json={"command": "ON"}).status_code == 404
        assert client.post("/api/v1/devices/FAN-01/commands", json={"command": "OPEN"}).status_code == 400


def test_command_without_mqtt_is_refused(token) -> None:
    with _client(token) as client:
        assert client.post("/api/v1/devices/FAN-01/commands", json={"command": "ON"}).status_code == 503


def test_command_goes_to_core_and_state_waits_for_completion(api) -> None:
    client, fake, bridge = api
    response = client.post("/api/v1/devices/LED-LIVING-01/commands", json={"command": "on"})
    assert response.status_code == 202
    body = response.json()
    assert body["device"]["state"] == "UNKNOWN"  # no optimistic change
    assert body["device"]["pending_command"] == "ON"
    [(topic, request)] = [item for item in fake.published if item[0] == TASK_REQUEST]
    assert topic == "pfn/v1/core/task/request"
    _validator("task-request.schema.json").validate(request)
    _validator("device-control.schema.json").validate(request["parameters"])
    assert request["parameters"] == {"target_device_id": "LED-LIVING-01", "action": "set_switch",
                                     "parameters": {"on": True}, "origin": "ui", "ai_device": None, "ai_action": None}
    assert request["source_node_id"] in {"NODE-RPI4-01", "NODE-RPI5-01"}
    assert not any(topic.startswith("homemind/") or "/device/" in topic for topic, _ in fake.published)
    assert TASK_UPDATE in fake.subscribed

    bridge.handle_task_update(_update(request["task_id"], "running", 2))
    device = client.get("/api/v1/dashboard").json()["snapshot"]["devices"]
    living = next(item for item in device if item["id"] == "LED-LIVING-01")
    assert living["state"] == "UNKNOWN" and living["last_task_state"] == "running"
    bridge.handle_task_update(_update(request["task_id"], "completed", 3))
    living = next(item for item in client.get("/api/v1/devices").json()["devices"] if item["id"] == "LED-LIVING-01")
    assert living["state"] == "ON" and living["pending_command"] is None
    status = client.get(f"/api/v1/tasks/{request['task_id']}").json()
    assert status["state"] == "completed"


def test_servo_uses_registry_positions_and_door_open_needs_approval(api) -> None:
    client, fake, bridge = api
    response = client.post("/api/v1/devices/SERVO-DOOR-01/commands", json={"command": "OPEN"})
    assert response.status_code == 202 and "승인" in response.json()["message"]
    request = fake.published[-1][1]
    assert request["parameters"]["action"] == "set_position"
    assert request["parameters"]["parameters"] == {"position_degrees": 90}
    assert request["safety_level"] == "caution"
    _validator("task-request.schema.json").validate(request)
    client.post("/api/v1/devices/SERVO-WINDOW-01/commands", json={"command": "CLOSE"})
    assert fake.published[-1][1]["parameters"]["parameters"] == {"position_degrees": 0}


def test_rejected_task_keeps_state_and_reports_failure(api) -> None:
    client, fake, bridge = api
    client.post("/api/v1/devices/FAN-01/commands", json={"command": "ON"})
    task_id = fake.published[-1][1]["task_id"]
    bridge.handle_task_update(_update(task_id, "rejected", 2, {"code": "DEVICE_NOT_WIRED", "message": "x",
                                                               "occurred_at_ms": 1, "retry_allowed": False}))
    fan = next(item for item in client.get("/api/v1/devices").json()["devices"] if item["id"] == "FAN-01")
    assert fan["state"] == "UNKNOWN" and fan["last_failure_code"] == "DEVICE_NOT_WIRED"
    assert client.get(f"/api/v1/tasks/{task_id}").json()["failure_code"] == "DEVICE_NOT_WIRED"
    # stale revision is ignored
    bridge.handle_task_update(_update(task_id, "completed", 1))
    assert client.get(f"/api/v1/tasks/{task_id}").json()["state"] == "rejected"


def test_password_only_from_file(monkeypatch, tmp_path) -> None:
    monkeypatch.setenv("HOMEMIND_MQTT_PASSWORD", "x")
    with pytest.raises(RuntimeError):
        BridgeConfig.from_env()
    monkeypatch.delenv("HOMEMIND_MQTT_PASSWORD")
    secret = tmp_path / "pw"
    secret.write_text("value\n", encoding="utf-8")
    monkeypatch.setenv("HOMEMIND_MQTT_PASSWORD_FILE", str(secret))
    assert BridgeConfig.from_env().password() == "value"
    monkeypatch.setenv("HOMEMIND_SOURCE_NODE_ID", "NODE-RPI5-02")
    with pytest.raises(RuntimeError):
        BridgeConfig.from_env()


def _broker_up(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=1):
            return True
    except OSError:
        return False


@pytest.mark.skipif(not _broker_up("127.0.0.1", 1883) or os.getenv("HOMEMIND_E2E") != "1",
                    reason="set HOMEMIND_E2E=1 with demo/pc docker compose (core + fake ESP) running")
def test_e2e_against_pc_core(monkeypatch, token) -> None:
    """PC simulation: real Mosquitto + homemind-core (x86 Docker) + fake ESP32. Not hardware."""

    store = HardwareStore()

    def on_task(task):
        if task.state == "completed":
            store.confirm_command(task.device_id, task.command)
        store.task_changed(task.device_id, task.task_id, task.state, (task.failure or {}).get("code"))

    bridge = CoreBridge(BridgeConfig(enabled=True, client_id=f"homemind-mobile-api-test-{uuid.uuid4().hex[:6]}"),
                        on_task=on_task, on_nodes=store.set_node_status)
    monkeypatch.setattr(main, "store", store)
    monkeypatch.setattr(main, "core", bridge)
    with _client(token) as client:
        for _ in range(50):
            if bridge.connected:
                break
            time.sleep(0.1)
        results = {}
        for device_id, command in (("SERVO-WINDOW-01", "OPEN"), ("SERVO-WINDOW-01", "CLOSE"), ("FAN-01", "ON"),
                                   ("LED-LIVING-01", "ON"), ("SERVO-DOOR-01", "OPEN")):
            task_id = client.post(f"/api/v1/devices/{device_id}/commands", json={"command": command}).json()["task_id"]
            deadline = time.time() + 8
            state = "requested"
            while time.time() < deadline:
                state = client.get(f"/api/v1/tasks/{task_id}").json()["state"]
                if state in {"completed", "failed", "rejected", "proposed"}:
                    break
                time.sleep(0.2)
            results[(device_id, command)] = state
    assert results[("SERVO-WINDOW-01", "OPEN")] == "completed"
    assert results[("SERVO-WINDOW-01", "CLOSE")] == "completed"
    assert results[("FAN-01", "ON")] == "completed"
    assert results[("SERVO-DOOR-01", "OPEN")] == "proposed"  # waits for the 7-inch UI approval
    fan = next(item for item in store.devices() if item.id == "FAN-01")
    assert fan.state == "ON"
