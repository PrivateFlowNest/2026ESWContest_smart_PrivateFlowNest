"""pfn.v1 message builders and contract validation (contracts/pfn.v1)."""

from __future__ import annotations

import json
import time
import uuid
from functools import lru_cache
from pathlib import Path
from typing import Any, Mapping

from jsonschema import Draft202012Validator, FormatChecker

from .config import CONTRACTS_DIR

TOPICS = {
    "perception": "pfn/v1/ai/perception",
    "task_request": "pfn/v1/core/task/request",
    "task_update": "pfn/v1/core/task/update",
    "llm_request": "pfn/v1/ai/llm/request",
    "llm_result": "pfn/v1/ai/llm/result",
}


def now_ms() -> int:
    return int(time.time() * 1000)


def new_id() -> str:
    return str(uuid.uuid4())


@lru_cache(maxsize=None)
def _validator(schema_name: str, contracts_dir: str) -> Draft202012Validator:
    schema = json.loads((Path(contracts_dir) / "pfn.v1" / schema_name).read_text(encoding="utf-8"))
    return Draft202012Validator(schema, format_checker=FormatChecker())


def validate(document: Mapping[str, Any], schema_name: str, contracts_dir: Path = CONTRACTS_DIR) -> None:
    errors = sorted(_validator(schema_name, str(contracts_dir)).iter_errors(document), key=str)
    if errors:
        # Location and keyword only: jsonschema messages quote the offending value, which can be the
        # transcript of an llm/request, and this text ends up in logs.
        raise ValueError(f"{schema_name}: " + "; ".join(
            f"{'/'.join(str(part) for part in error.absolute_path) or '<root>'} fails {error.validator}"
            for error in errors[:5]))


def task_request(
    *,
    source_node_id: str,
    intent: str,
    target_device: str,
    parameters: Mapping[str, Any],
    confidence: float,
    trace_id: str | None = None,
    safety_level: str = "normal",
) -> dict[str, Any]:
    document = {
        "schema_version": "pfn.v1",
        "event_id": new_id(),
        "trace_id": trace_id or new_id(),
        "task_id": new_id(),
        "source_node_id": source_node_id,
        "intent": intent,
        "target_device": target_device,
        "priority": 50,
        "safety_level": safety_level,
        "interruptible": True,
        "resume_policy": "never",
        "confidence": round(max(0.0, min(1.0, float(confidence))), 4),
        "parameters": dict(parameters),
        "requested_at_ms": now_ms(),
    }
    validate(document, "task-request.schema.json")
    if intent == "device_control":
        validate(document["parameters"], "device-control.schema.json")
    return document


def llm_request(
    *,
    text: str,
    utterance_id: str,
    stt_model_version: str,
    stt_confidence: float | None,
    parser_result: str,
    context: Mapping[str, Any] | None,
    ttl_ms: int,
    trace_id: str | None = None,
    simulated: bool = False,
) -> dict[str, Any]:
    requested = now_ms()
    document: dict[str, Any] = {
        "schema_version": "pfn.v1",
        "event_id": new_id(),
        "trace_id": trace_id or new_id(),
        "request_id": new_id(),
        "source_node_id": "NODE-RPI5-02",
        "utterance_id": utterance_id,
        "transcript": {
            "text": text[:500],
            "language": "ko",
            "stt_model_version": stt_model_version,
            "stt_confidence": stt_confidence,
        },
        "parser": {"result": parser_result, "rule_version": "guardrails-2026-10-22"},
        "requested_at_ms": requested,
        "expires_at_ms": requested + ttl_ms,
    }
    if context:
        document["context"] = dict(context)
    if simulated:
        document["simulated"] = True
    validate(document, "llm-request.schema.json")
    return document


def llm_result(
    *,
    request: Mapping[str, Any],
    status: str,
    task_id: str | None,
    proposal: Mapping[str, Any] | None,
    origin: str,
    latency_ms: float,
    model_name: str,
    simulated: bool = False,
) -> dict[str, Any]:
    proposal = proposal or {}
    document: dict[str, Any] = {
        "schema_version": "pfn.v1",
        "event_id": new_id(),
        "trace_id": request["trace_id"],
        "request_id": request["request_id"],
        "source_node_id": "NODE-RPI5-01",
        "status": status,
        "task_id": task_id,
        "ai_intent": proposal.get("intent"),
        "ai_device": proposal.get("device"),
        "ai_action": proposal.get("action"),
        "safety_flags": list(proposal.get("safety_flags", [])),
        "clarification_question": proposal.get("clarification_question"),
        "model": {"name": model_name, "origin": origin, "latency_ms": round(float(latency_ms), 3)},
        "completed_at_ms": now_ms(),
    }
    if simulated:
        document["simulated"] = True
    validate(document, "llm-result.schema.json")
    return document


def perception(
    *,
    observation_type: str,
    zone: str,
    confidence: float,
    attributes: Mapping[str, Any],
    source_device_id: str | None = None,
    trace_id: str | None = None,
    simulated: bool = False,
) -> dict[str, Any]:
    document: dict[str, Any] = {
        "schema_version": "pfn.v1",
        "event_id": new_id(),
        "trace_id": trace_id or new_id(),
        "source_node_id": "NODE-RPI5-02",
        "observation_type": observation_type,
        "zone": zone,
        "confidence": round(max(0.0, min(1.0, float(confidence))), 4),
        "attributes": dict(attributes),
        "observed_at_ms": now_ms(),
    }
    if source_device_id:
        document["source_device_id"] = source_device_id
    if simulated:
        document["simulated"] = True
    validate(document, "perception.schema.json")
    return document
