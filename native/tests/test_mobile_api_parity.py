"""Mobile API parity + E2E: Python reference (FastAPI) vs C++ homemind-mobile-api. SIMULATION (PC), not a device.

    PC Mosquitto + homemind-core + fake ESP32/ESP8266     demo/pc docker compose (broker 127.0.0.1:1883)
    Python Mobile API 0.4 (uvicorn, Windows)              team/homemind-application-2026-10-21/backend/.venv
    C++ homemind-mobile-api (x86 Docker build)            container, host port 127.0.0.1:<free>

Both servers get the same scripted HTTP requests; status codes, the important headers and JSON bodies must match
(ids, tokens and timestamps excluded). Both share one pairing secret, so a QR printed by the C++ CLI is claimed on
the Python server and the other way round (same HMAC code algorithm). Device commands go through the real PC core
to the fake ESPs; every task request the two servers publish is checked against the pfn.v1 schemas. Neither log
may contain a token, a pairing code or the secret.

    services/runtime/.venv/Scripts/python -m pytest native/tests/test_mobile_api_parity.py -s
"""

from __future__ import annotations

import hashlib
import hmac
import base64
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import IMAGE, REPO, build, validate_topic_message  # noqa: E402

BUILDER = "mobile"
BACKEND = REPO / "team" / "homemind-application-2026-10-21" / "backend"
PY_EXE = BACKEND / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
BINARY = "/build/services/homemind-mobile-api/homemind-mobile-api"
CPP_DATA = "/var/lib/hm-mobile"  # container-local (a Windows bind mount has no POSIX modes / flock)
VOLATILE = {"task_id", "pending_task_id", "updated_at", "token", "token_id"}
# C++ 0.5 = Python 0.4 + phone approvals and events (not in the Python reference); the shared API must stay the same.
VERSIONED = {"version"}
FINAL = {"completed", "failed", "rejected", "cancelled", "proposed"}
DEVICE_NAME = "HomeMind parity"


def _port_open(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _docker(*args: str, check: bool = True, timeout: float = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["docker", *args], capture_output=True, text=True, encoding="utf-8", check=check,
                          timeout=timeout, env={**os.environ, "MSYS_NO_PATHCONV": "1"})


def call(base: str, method: str, path: str, body: Any = None, *, token: str | None = None,
         authorization: str | None = None, raw: bytes | None = None,
         content_type: str = "application/json") -> tuple[int, Any, dict[str, str]]:
    data = raw if raw is not None else (json.dumps(body, ensure_ascii=False).encode() if body is not None else None)
    request = urllib.request.Request(base + path, data=data, method=method)
    if data is not None and content_type:
        request.add_header("Content-Type", content_type)
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    if authorization:
        request.add_header("Authorization", authorization)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            status, payload, headers = response.status, response.read(), response.headers
    except urllib.error.HTTPError as error:
        status, payload, headers = error.code, error.read(), error.headers
    text = payload.decode("utf-8")
    return status, (json.loads(text) if text else None), {k.lower(): v for k, v in headers.items()}


def norm(value: Any) -> Any:
    if isinstance(value, dict):
        skip = VOLATILE | (VERSIONED if value.get("service") == "homemind-mobile-api" else set())
        out = {k: norm(v) for k, v in value.items() if k not in skip}
        if out.get("type") == "json_invalid":  # parser-specific position and message
            out.pop("loc", None)
            out.pop("ctx", None)
        return out
    if isinstance(value, list):
        return [norm(item) for item in value]
    return value


class Observer:
    """Records every pfn/v1/core/task/request on the PC broker (schema check of what both APIs publish)."""

    def __init__(self) -> None:
        import paho.mqtt.client as mqtt

        self.lock = threading.Lock()
        self.requests: dict[str, dict[str, Any]] = {}
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"parity-obs-{uuid.uuid4().hex[:6]}")
        self.client.on_connect = lambda c, u, f, rc, p: c.subscribe("pfn/v1/core/task/request", qos=1)
        self.client.on_message = self._on_message
        self.client.connect("127.0.0.1", 1883, 30)
        self.client.loop_start()

    def _on_message(self, client, userdata, message) -> None:
        document = json.loads(message.payload.decode("utf-8"))
        with self.lock:
            self.requests[document.get("task_id", "")] = document

    def get(self, task_id: str, timeout: float = 5) -> dict[str, Any] | None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if task_id in self.requests:
                    return self.requests[task_id]
            time.sleep(0.05)
        return None

    def stop(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()


@pytest.fixture(scope="module")
def servers(tmp_path_factory):
    if not _port_open(1883):
        pytest.skip("PC demo broker not running (docker compose -f demo/pc/docker-compose.yml --profile fake-esp up -d)")
    if not PY_EXE.exists():
        pytest.skip(f"backend venv missing: {PY_EXE}")
    build(BUILDER, target="homemind-mobile-api")
    work = tmp_path_factory.mktemp("mobile-parity")
    tag = uuid.uuid4().hex[:6]
    cpp_port, py_port = _free_port(), _free_port()
    cpp_base, py_base = f"http://127.0.0.1:{cpp_port}", f"http://127.0.0.1:{py_port}"
    (work / "cpp.json").write_text(json.dumps({
        "broker": {"host": "host.docker.internal", "port": 1883}, "contracts_dir": "/w/contracts",
        "device_registry": "/w/contracts/pfn.v1/device-registry.json", "mqtt_client_id": f"hm-mobile-parity-cpp-{tag}",
        "listen_host": "0.0.0.0", "listen_port": 8000, "data_dir": CPP_DATA, "public_base_url": cpp_base,
        "device_name": DEVICE_NAME}), "utf-8")
    name = f"hm-mobile-parity-{tag}"
    _docker("run", "-d", "--rm", "--name", name, "-p", f"127.0.0.1:{cpp_port}:8000", "-v", f"{REPO.as_posix()}:/w",
            "-v", f"hm-native-build-{BUILDER}:/build", "-v", f"{work.as_posix()}:/t", IMAGE, BINARY,
            "--config", "/t/cpp.json")
    py_data = work / "py-data"
    py_log = (work / "python.log").open("w", encoding="utf-8")
    python = None
    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                if call(cpp_base, "GET", "/api/v1/health")[1]["mqtt_connected"]:
                    break
            except OSError:
                pass
            time.sleep(0.3)
        # one pairing secret for both servers (random test value, temp dir only)
        secret = _docker("exec", name, "cat", f"{CPP_DATA}/pairing-secret").stdout
        py_data.mkdir()
        (py_data / "pairing-secret").write_text(secret, "ascii")
        env = {**os.environ, "HOMEMIND_MQTT_ENABLED": "true", "HOMEMIND_MQTT_HOST": "127.0.0.1",
               "HOMEMIND_MQTT_PORT": "1883", "HOMEMIND_MQTT_CLIENT_ID": f"hm-mobile-parity-py-{tag}",
               "HOMEMIND_SOURCE_NODE_ID": "NODE-RPI4-01", "HOMEMIND_DATA_DIR": str(py_data),
               "HOMEMIND_PUBLIC_BASE_URL": py_base, "HOMEMIND_DEVICE_NAME": DEVICE_NAME, "PYTHONIOENCODING": "utf-8"}
        for key in ("HOMEMIND_MQTT_USERNAME", "HOMEMIND_MQTT_PASSWORD_FILE", "HOMEMIND_DEVICE_REGISTRY"):
            env.pop(key, None)
        python = subprocess.Popen([str(PY_EXE), "-m", "uvicorn", "app.main:app", "--host", "127.0.0.1", "--port",
                                   str(py_port)], cwd=BACKEND, env=env, stdout=py_log, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                if call(py_base, "GET", "/api/v1/health")[1]["mqtt_connected"]:
                    break
            except OSError:
                pass
            time.sleep(0.3)
        time.sleep(1.0)  # both received core's state/response
        observer = Observer()
        yield {"cpp": cpp_base, "py": py_base, "name": name, "env": env, "work": work, "secret": secret.strip(),
               "observer": observer}
        observer.stop()
    finally:
        if python is not None:
            python.terminate()
            python.wait(timeout=10)
        py_log.close()
        logs = _docker("logs", name, check=False)
        (work / "cpp.log").write_text(logs.stdout + logs.stderr, "utf-8")
        _docker("rm", "-f", name, check=False)


def _cli_cpp(servers, *args: str) -> str:
    return _docker("exec", servers["name"], BINARY, "--config", "/t/cpp.json", *args).stdout


def _cli_py(servers, *args: str) -> str:
    return subprocess.run([str(PY_EXE), "-m", "app.cli", *args], cwd=BACKEND, env=servers["env"], check=True,
                          capture_output=True, text=True, encoding="utf-8").stdout


def _code(secret_hex: str, nonce: str) -> str:
    mac = hmac.new(bytes.fromhex(secret_hex), b"homemind-pairing-v1\n" + nonce.encode(), hashlib.sha256).digest()
    code = base64.b32encode(mac[:5]).decode()
    return f"{code[:4]}-{code[4:]}"


def _poll(base: str, token: str, task_id: str, timeout: float = 10) -> dict[str, Any]:
    deadline = time.time() + timeout
    body: dict[str, Any] = {}
    while time.time() < deadline:
        _, body, _ = call(base, "GET", f"/api/v1/tasks/{task_id}", token=token)
        if body.get("state") in FINAL:
            return body
        time.sleep(0.2)
    return body


def test_python_and_cpp_mobile_api_agree(servers):
    cpp, py, observer = servers["cpp"], servers["py"], servers["observer"]
    report: list[dict[str, Any]] = []
    mismatches: list[str] = []
    issued: list[str] = []

    def compare(label: str, py_result, cpp_result, headers=()) -> None:
        a = (py_result[0], norm(py_result[1]), {h: py_result[2].get(h) for h in headers})
        b = (cpp_result[0], norm(cpp_result[1]), {h: cpp_result[2].get(h) for h in headers})
        report.append({"step": label, "status": a[0], "equal": a == b})
        if a != b:
            mismatches.append(f"{label}\n  python: {a}\n  c++:    {b}")

    # --- pairing: shared secret, cross-claimed QRs ------------------------------------------------------------
    qr_cpp = json.loads(_cli_cpp(servers, "--print-pairing-qr", "--json"))
    qr_py = json.loads(_cli_py(servers, "--print-pairing-qr", "--json"))
    for qr, base in ((qr_cpp, cpp), (qr_py, py)):
        assert set(qr) == {"version", "device_id", "device_name", "base_url", "pairing_code", "nonce", "expires_at"}
        assert qr["version"] == 1 and qr["base_url"] == base and qr["device_name"] == DEVICE_NAME
        assert qr["pairing_code"] == _code(servers["secret"], qr["nonce"])  # same algorithm as the test's hmac
    claim_py = call(py, "POST", "/api/v1/pairing/claim", {"code": qr_cpp["pairing_code"], "nonce": qr_cpp["nonce"],
                                                          "device_name": "parity phone"})
    claim_cpp = call(cpp, "POST", "/api/v1/pairing/claim", {"code": qr_py["pairing_code"], "nonce": qr_py["nonce"],
                                                            "device_name": "parity phone"})
    compare("claim (QR of the other implementation)", claim_py, claim_cpp)
    assert claim_py[0] == 201 and claim_cpp[0] == 201, (claim_py, claim_cpp)
    t_py, t_cpp = claim_py[1]["token"], claim_cpp[1]["token"]
    issued += [t_py, t_cpp]
    compare("claim again (one-time)",
            call(py, "POST", "/api/v1/pairing/claim", {"code": qr_cpp["pairing_code"], "nonce": qr_cpp["nonce"]}),
            call(cpp, "POST", "/api/v1/pairing/claim", {"code": qr_py["pairing_code"], "nonce": qr_py["nonce"]}))
    now = int(time.time())
    expired = f"v1.{now - 5}.{uuid.uuid4().hex}"
    forged = f"v1.{now + 3 * 3600}.{uuid.uuid4().hex}"
    for label, nonce in (("claim expired code", expired), ("claim far-future nonce", forged)):
        body = {"code": _code(servers["secret"], nonce), "nonce": nonce}
        compare(label, call(py, "POST", "/api/v1/pairing/claim", body), call(cpp, "POST", "/api/v1/pairing/claim", body))
    fresh = f"v1.{now + 300}.{uuid.uuid4().hex}"
    wrong = {"code": "AAAA-AAAA", "nonce": fresh}
    compare("claim wrong code", call(py, "POST", "/api/v1/pairing/claim", wrong),
            call(cpp, "POST", "/api/v1/pairing/claim", wrong))
    for label, kwargs in (
            ("claim {}", {"body": {}}),
            ("claim wrong types", {"body": {"code": 1, "nonce": None}}),
            ("claim too long", {"body": {"code": "A" * 33, "nonce": "x", "device_name": "가" * 65}}),
            ("claim not json", {"raw": b"code=1", "content_type": "application/x-www-form-urlencoded"}),
            ("claim bad json", {"raw": b"{bad"}),
            ("claim empty", {"raw": b""}),
    ):
        compare(label, call(py, "POST", "/api/v1/pairing/claim", **kwargs),
                call(cpp, "POST", "/api/v1/pairing/claim", **kwargs))

    # --- auth gate ---------------------------------------------------------------------------------------------
    www = ("www-authenticate",)
    for label, path, kwargs in (
            ("health (open)", "/api/v1/health", {}),
            ("dashboard without token", "/api/v1/dashboard", {}),
            ("devices with Basic auth", "/api/v1/devices", {"authorization": "Basic YTpi"}),
            ("map with a wrong token", "/api/v1/hardware-map", {"token": "hm1_" + "f" * 64}),
            ("unknown /api path without token", "/api/v1/nothing", {}),
            ("task without token", "/api/v1/tasks/x", {}),
    ):
        compare(label, call(py, "GET", path, **kwargs), call(cpp, "GET", path, **kwargs), www)
    compare("command without token", call(py, "POST", "/api/v1/devices/FAN-01/commands", {"command": "ON"}),
            call(cpp, "POST", "/api/v1/devices/FAN-01/commands", {"command": "ON"}), www)

    # --- read endpoints, routing and validation (authenticated) ----------------------------------------------
    for path in ("/api/v1/health", "/api/v1/hardware-map", "/api/v1/devices", "/api/v1/dashboard",
                 "/api/v1/nothing", "/api/v1/tasks/unknown"):
        compare(f"GET {path}", call(py, "GET", path, token=t_py), call(cpp, "GET", path, token=t_cpp))
    compare("GET on a POST route", call(py, "GET", "/api/v1/devices/FAN-01/commands", token=t_py),
            call(cpp, "GET", "/api/v1/devices/FAN-01/commands", token=t_cpp), ("allow",))
    compare("POST on a GET route", call(py, "POST", "/api/v1/devices", {}, token=t_py),
            call(cpp, "POST", "/api/v1/devices", {}, token=t_cpp), ("allow",))
    for label, device, kwargs in (
            ("unknown device", "HEATING-01", {"body": {"command": "ON"}}),
            ("read-only camera", "CAM-INDOOR-01", {"body": {"command": "ON"}}),
            ("not wired LED", "LED-GUIDE-01", {"body": {"command": "ON"}}),
            ("fan OPEN", "FAN-01", {"body": {"command": "OPEN"}}),
            ("missing command", "FAN-01", {"body": {}}),
            ("command not a string", "FAN-01", {"body": {"command": 1}}),
            ("text/plain body", "FAN-01", {"raw": b'{"command":"ON"}', "content_type": "text/plain"}),
            ("bad JSON", "FAN-01", {"raw": b"{bad"}),
            ("array body", "FAN-01", {"body": [1]}),
            ("validation before device lookup", "HEATING-01", {"body": {}}),
    ):
        compare(f"command: {label}", call(py, "POST", f"/api/v1/devices/{device}/commands", token=t_py, **kwargs),
                call(cpp, "POST", f"/api/v1/devices/{device}/commands", token=t_cpp, **kwargs))

    # --- commands through the PC core and fake ESPs ----------------------------------------------------------
    finals: dict[tuple[str, str], tuple[str, str]] = {}
    for device, command in (("SERVO-WINDOW-01", "OPEN"), ("SERVO-WINDOW-01", "CLOSE"), ("FAN-01", "ON"),
                            ("LED-LIVING-01", " on "), ("FAN-01", "off"), ("SERVO-DOOR-01", "OPEN")):
        results = {}
        for side, base, token in (("py", py, t_py), ("cpp", cpp, t_cpp)):
            accepted = call(base, "POST", f"/api/v1/devices/{device}/commands", {"command": command}, token=token)
            assert accepted[0] == 202, (side, device, command, accepted)
            task_id = accepted[1]["task_id"]
            published = observer.get(task_id)
            assert published, f"{side}: no task request on the broker for {task_id}"
            validate_topic_message("pfn/v1/core/task/request", published)
            assert published["parameters"]["origin"] == "ui" and published["source_node_id"] == "NODE-RPI4-01"
            results[side] = (accepted, _poll(base, token, task_id))
        compare(f"command {device} {command.strip()} (202)", results["py"][0], results["cpp"][0])
        compare(f"task {device} {command.strip()} (final)", (200, results["py"][1], {}), (200, results["cpp"][1], {}))
        finals[(device, command.strip().upper())] = (results["py"][1].get("state"), results["cpp"][1].get("state"))
    assert finals[("SERVO-WINDOW-01", "OPEN")] == ("completed", "completed"), finals
    assert finals[("SERVO-WINDOW-01", "CLOSE")] == ("completed", "completed"), finals
    assert finals[("FAN-01", "ON")] == ("completed", "completed"), finals
    assert finals[("FAN-01", "OFF")] == ("completed", "completed"), finals
    assert finals[("SERVO-DOOR-01", "OPEN")] == ("proposed", "proposed"), finals  # waits for the 7-inch UI
    time.sleep(0.5)
    for path in ("/api/v1/devices", "/api/v1/dashboard", "/api/v1/hardware-map"):
        compare(f"GET {path} after commands", call(py, "GET", path, token=t_py), call(cpp, "GET", path, token=t_cpp))
    devices = {d["id"]: d for d in call(cpp, "GET", "/api/v1/devices", token=t_cpp)[1]["devices"]}
    assert devices["SERVO-WINDOW-01"]["state"] == "CLOSED" and devices["FAN-01"]["state"] == "OFF"
    assert devices["SERVO-DOOR-01"]["pending_command"] == "OPEN"

    # --- revocation with each CLI ------------------------------------------------------------------------------
    assert claim_cpp[1]["token_id"] in _cli_cpp(servers, "--list-tokens")
    assert claim_py[1]["token_id"] in _cli_py(servers, "--list-tokens")
    _cli_cpp(servers, "--revoke-token", claim_cpp[1]["token_id"])
    _cli_py(servers, "--revoke-token", claim_py[1]["token_id"])
    compare("revoked token", call(py, "GET", "/api/v1/devices", token=t_py),
            call(cpp, "GET", "/api/v1/devices", token=t_cpp), www)

    # --- failed-claim rate limit (same history on both: same point of refusal) ------------------------------
    def hammer(base: str) -> list[tuple[int, bool]]:
        out = []
        for _ in range(8):
            status, _, headers = call(base, "POST", "/api/v1/pairing/claim", wrong)
            out.append((status, "retry-after" in headers))
        return out

    py_limit, cpp_limit = hammer(py), hammer(cpp)
    report.append({"step": "rate limit sequence", "python": py_limit, "cpp": cpp_limit})
    assert py_limit == cpp_limit, (py_limit, cpp_limit)
    assert (429, True) in py_limit and py_limit[-1] == (429, True)
    compare("claim while limited (body)", call(py, "POST", "/api/v1/pairing/claim", wrong),
            call(cpp, "POST", "/api/v1/pairing/claim", wrong))

    # --- nothing secret in the logs ------------------------------------------------------------------------------
    cpp_log = _docker("logs", servers["name"], check=False)
    py_log = (servers["work"] / "python.log").read_text("utf-8")
    secrets_seen = [t for t in issued] + [qr_cpp["pairing_code"], qr_py["pairing_code"], servers["secret"]]
    for text, label in ((cpp_log.stdout + cpp_log.stderr, "c++"), (py_log, "python")):
        for value in secrets_seen:
            assert value not in text, f"{label} log contains a secret value"

    out = servers["work"] / "parity-report.json"
    out.write_text(json.dumps(report, ensure_ascii=False, indent=1), "utf-8")
    equal = sum(1 for item in report if item.get("equal"))
    print(f"\nmobile API parity (PC simulation): {equal}/{sum(1 for i in report if 'equal' in i)} steps equal, "
          f"commands {finals}, rate limit {py_limit}; report {out}")
    assert not mismatches, "\n".join(mismatches)
