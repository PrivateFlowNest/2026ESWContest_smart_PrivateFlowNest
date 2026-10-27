from __future__ import annotations

import logging
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from .auth import AuthConfig, PairingService, bearer_token, now_ms, requires_token
from .core_bridge import BridgeConfig, CoreBridge, TrackedTask
from .hardware_models import (
    DashboardResponse,
    DeviceCommandRequest,
    DeviceCommandResponse,
    HardwareMapResponse,
    HealthResponse,
    PairingClaimRequest,
    PairingClaimResponse,
    TaskStatusResponse,
)
from .hardware_store import HardwareStore

log = logging.getLogger(__name__)
store = HardwareStore()


def _on_task(task: TrackedTask) -> None:
    if task.state == "completed":
        store.confirm_command(task.device_id, task.command)
    store.task_changed(task.device_id, task.task_id, task.state, (task.failure or {}).get("code"))


def _make_bridge() -> CoreBridge:
    return CoreBridge(BridgeConfig.from_env(), on_task=_on_task, on_nodes=store.set_node_status)


core = _make_bridge()
# QR pairing + bearer tokens (app/auth.py; same files and algorithm as the C++ homemind-mobile-api).
auth = PairingService(AuthConfig.from_env())


@asynccontextmanager
async def lifespan(_: FastAPI):
    auth.prepare()
    core.start()
    yield
    core.stop()


app = FastAPI(
    title="HomeMind Mobile API",
    version="0.4.0-beta",
    description="Flutter 앱과 RPI4-01 core 사이의 API 경계. 장치 명령은 core(task/request)를 거친다. "
                "/api/v1/health와 /api/v1/pairing/claim 외에는 페어링 토큰(Authorization: Bearer)이 필요하다.",
    lifespan=lifespan,
)
# The Android app does not need CORS. Browser clients must be listed explicitly (dev network only).
_origins = [item for item in os.getenv("HOMEMIND_CORS_ORIGINS", "").split(",") if item]
if _origins:
    app.add_middleware(CORSMiddleware, allow_origins=_origins, allow_credentials=False,
                       allow_methods=["GET", "POST", "OPTIONS"], allow_headers=["Content-Type", "Authorization"])


@app.middleware("http")
async def require_token(request: Request, call_next):
    """Bearer token on every /api/ call except health and the pairing claim; checked before routing."""

    if requires_token(request.method, request.url.path):
        token = bearer_token(request.headers.get("authorization", ""))
        if token is None:
            return JSONResponse({"detail": "missing bearer token"}, status_code=401,
                                headers={"WWW-Authenticate": "Bearer"})
        if auth.tokens.verify(token) is None:
            client = request.client.host if request.client else "unknown"
            log.info("refused token from %s for %s", client, request.url.path)
            return JSONResponse({"detail": "invalid or revoked token"}, status_code=401,
                                headers={"WWW-Authenticate": 'Bearer error="invalid_token"'})
    return await call_next(request)


@app.get("/api/v1/health", response_model=HealthResponse)
def health() -> HealthResponse:
    return HealthResponse(mqtt_enabled=core.enabled, mqtt_connected=core.connected)


@app.get("/api/v1/dashboard", response_model=DashboardResponse)
def dashboard() -> DashboardResponse:
    return DashboardResponse(snapshot=store.snapshot(connected=core.connected))


@app.get("/api/v1/hardware-map", response_model=HardwareMapResponse)
def hardware_map() -> HardwareMapResponse:
    nodes, controllers, devices = store.hardware_map()
    return HardwareMapResponse(nodes=nodes, controllers=controllers, devices=devices)


@app.get("/api/v1/devices")
def list_devices():
    return {"devices": store.devices()}


@app.post("/api/v1/devices/{device_id}/commands", response_model=DeviceCommandResponse, status_code=202)
def send_device_command(device_id: str, request: DeviceCommandRequest) -> DeviceCommandResponse:
    if store.device(device_id) is None:
        raise HTTPException(status_code=404, detail="device ID is not in device-registry.json")
    command = request.command.strip().upper()
    try:
        control = store.registry.control(device_id, command)
    except ValueError as error:
        raise HTTPException(status_code=400, detail=str(error)) from error
    task = core.request(control, command)
    if task is None:
        raise HTTPException(status_code=503, detail="core MQTT connection is not available")
    device = store.mark_pending(device_id, command, task.task_id)
    needs_approval = control.device_id == "SERVO-DOOR-01" and control.position_ref == "open"
    message = ("현관문 열기는 7인치 화면에서 승인해야 실행됩니다." if needs_approval
               else f"{device.name} {command} 요청을 core에 보냈습니다. 장치 응답(ACK) 후 상태가 바뀝니다.")
    return DeviceCommandResponse(device=device, accepted=True, published=True, message=message,
                                 task_id=task.task_id, task_state=task.state)


@app.get("/api/v1/tasks/{task_id}", response_model=TaskStatusResponse)
def task_status(task_id: str) -> TaskStatusResponse:
    task = core.task(task_id)
    if task is None:
        raise HTTPException(status_code=404, detail="unknown or expired task")
    failure = task.failure or {}
    return TaskStatusResponse(task_id=task.task_id, device_id=task.device_id, command=task.command, state=task.state,
                              failure_code=failure.get("code"), failure_message=failure.get("message"))


@app.post("/api/v1/pairing/claim", response_model=PairingClaimResponse, status_code=201)
def pairing_claim(body: PairingClaimRequest, request: Request) -> PairingClaimResponse:
    """One-time code from the device's QR (homemind-mobile-api --print-pairing-qr / python -m app.cli) -> token."""

    client = request.client.host if request.client else "unknown"
    result = auth.claim(body.code, body.nonce, body.device_name, client, now_ms())
    if result.status == "rate_limited":
        raise HTTPException(status_code=429, detail="too many failed pairing attempts; try again later",
                            headers={"Retry-After": str(result.retry_after_s)})
    if result.status != "ok" or result.record is None or result.token is None:
        raise HTTPException(status_code=401, detail="invalid, expired or already used pairing code")
    return PairingClaimResponse(token=result.token, token_id=result.record.id, device_id=auth.config.device_id,
                                device_name=auth.config.device_name)


@app.get("/")
def root():
    return {"service": "HomeMind Mobile API", "docs": "/docs", "hardware_map": "/api/v1/hardware-map",
            "devices": "/api/v1/devices"}
