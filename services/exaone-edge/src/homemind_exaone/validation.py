"""Strict structural and semantic validation for untrusted LLM proposals."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping


class ProposalValidationError(ValueError):
    def __init__(self, errors: list[str]) -> None:
        self.errors = tuple(errors)
        super().__init__("; ".join(errors))


def workspace_root() -> Path:
    return Path(__file__).resolve().parents[4]


def default_schema_path() -> Path:
    return workspace_root() / "contracts" / "rpi5-internal" / "task-proposal.schema.json"


def default_capabilities_path() -> Path:
    return Path(__file__).resolve().parents[2] / "config" / "device-capabilities.json"


def default_activity_context_schema_path() -> Path:
    return workspace_root() / "contracts" / "rpi5-internal" / "activity-context.schema.json"


def default_activity_context() -> dict[str, Any]:
    return {
        "presence_zone": "UNKNOWN",
        "activity": "UNKNOWN",
        "audio_context": "UNKNOWN",
        "quiet_required": False,
        "confidence": 0.0,
    }


def load_json_object(path: str | Path) -> dict[str, Any]:
    source = Path(path)
    value = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {source}")
    return value


def _matches_type(value: object, allowed: str | list[str]) -> bool:
    names = [allowed] if isinstance(allowed, str) else allowed
    for name in names:
        if name == "null" and value is None:
            return True
        if name == "string" and isinstance(value, str):
            return True
        if name == "boolean" and isinstance(value, bool):
            return True
        if name == "object" and isinstance(value, dict):
            return True
        if name == "array" and isinstance(value, list):
            return True
        if name == "integer" and isinstance(value, int) and not isinstance(value, bool):
            return True
        if name == "number" and isinstance(value, (int, float)) and not isinstance(value, bool):
            return True
    return False


def _validate_value(name: str, value: object, rule: Mapping[str, Any]) -> list[str]:
    errors: list[str] = []
    if "type" in rule and not _matches_type(value, rule["type"]):
        return [f"{name}: invalid type"]
    if "enum" in rule and value not in rule["enum"]:
        errors.append(f"{name}: unsupported value {value!r}")
    if isinstance(value, str) and "maxLength" in rule:
        if len(value) > int(rule["maxLength"]):
            errors.append(f"{name}: exceeds maxLength")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in rule and value < float(rule["minimum"]):
            errors.append(f"{name}: below minimum")
        if "maximum" in rule and value > float(rule["maximum"]):
            errors.append(f"{name}: above maximum")
    return errors


def _validate_structure(
    proposal: object,
    schema: Mapping[str, Any],
) -> list[str]:
    if not isinstance(proposal, dict):
        return ["proposal: expected object"]
    errors: list[str] = []
    required = set(schema.get("required", []))
    properties = schema.get("properties", {})
    missing = sorted(required - proposal.keys())
    unknown = sorted(proposal.keys() - properties.keys())
    errors.extend(f"missing field: {name}" for name in missing)
    if schema.get("additionalProperties") is False:
        errors.extend(f"unknown field: {name}" for name in unknown)

    for name, value in proposal.items():
        rule = properties.get(name)
        if not isinstance(rule, dict):
            continue
        errors.extend(_validate_value(name, value, rule))
        if isinstance(value, list):
            if rule.get("uniqueItems") and len(value) != len(set(value)):
                errors.append(f"{name}: duplicate values")
            item_rule = rule.get("items", {})
            for index, item in enumerate(value):
                errors.extend(_validate_value(f"{name}[{index}]", item, item_rule))
        if isinstance(value, dict):
            nested = rule.get("properties", {})
            if rule.get("additionalProperties") is False:
                for extra in sorted(value.keys() - nested.keys()):
                    errors.append(f"{name}: unknown field {extra}")
            for child_name, child_value in value.items():
                child_rule = nested.get(child_name)
                if isinstance(child_rule, dict):
                    errors.extend(
                        _validate_value(
                            f"{name}.{child_name}", child_value, child_rule
                        )
                    )
    return errors


def _validate_semantics(
    proposal: Mapping[str, Any],
    capabilities: Mapping[str, Any],
) -> list[str]:
    errors: list[str] = []
    intent = proposal.get("intent")
    device = proposal.get("device")
    action = proposal.get("action")
    parameters = proposal.get("parameters")
    ambiguous = proposal.get("ambiguous")
    question = proposal.get("clarification_question")
    flags = proposal.get("safety_flags")

    if not isinstance(parameters, dict) or not isinstance(flags, list):
        return errors

    if ambiguous is True:
        if intent != "CLARIFY":
            errors.append("ambiguous=true requires intent=CLARIFY")
        if not isinstance(question, str) or not question.strip():
            errors.append("CLARIFY requires a non-empty clarification_question")
    elif intent == "CLARIFY":
        errors.append("intent=CLARIFY requires ambiguous=true")
    elif question is not None:
        errors.append("unambiguous proposals require clarification_question=null")

    actionable = {"DEVICE_CONTROL", "DEVICE_QUERY", "CONTEXT_SUGGESTION"}
    if intent in actionable:
        if not isinstance(device, str) or not isinstance(action, str):
            errors.append("actionable intent requires device and action")
        else:
            device_info = capabilities.get("devices", {}).get(device)
            if not isinstance(device_info, dict) or not device_info.get("enabled"):
                errors.append(f"device is unavailable: {device}")
            elif action not in device_info.get("actions", []):
                errors.append(f"action {action} is not allowed for {device}")
    else:
        if device is not None or action is not None or parameters:
            errors.append("CLARIFY/NO_ACTION must not contain device, action, or parameters")

    if intent == "DEVICE_QUERY" and action != "QUERY_STATUS":
        errors.append("DEVICE_QUERY requires action=QUERY_STATUS")
    if action == "QUERY_STATUS" and intent != "DEVICE_QUERY":
        errors.append("QUERY_STATUS requires intent=DEVICE_QUERY")
    if intent == "CONTEXT_SUGGESTION" and (
        device != "ALL_QUIET_DEVICES"
        or action != "SET_QUIET_MODE"
    ):
        errors.append(
            "CONTEXT_SUGGESTION requires ALL_QUIET_DEVICES/SET_QUIET_MODE"
        )

    required_parameter = {
        "SET_BRIGHTNESS": "brightness",
        "SET_VOLUME": "volume",
        "SET_LEVEL": "level",
        "SET_TEMPERATURE": "temperature_celsius",
    }.get(action)
    if required_parameter and required_parameter not in parameters:
        errors.append(f"{action} requires parameters.{required_parameter}")

    if "NEGATED_REQUEST" in flags and intent != "NO_ACTION":
        errors.append("NEGATED_REQUEST must use intent=NO_ACTION")
    clarify_only_flags = {
        "AMBIGUOUS_TARGET",
        "AMBIGUOUS_ACTION",
        "UNSUPPORTED_DEVICE",
        "UNSUPPORTED_ACTION",
        "HIGH_RISK",
        "CONFLICTING_REQUEST",
        "MODEL_OUTPUT_INVALID",
        "MODEL_UNAVAILABLE",
    }
    invalid_for_intent = sorted(clarify_only_flags.intersection(flags))
    if invalid_for_intent and intent != "CLARIFY":
        errors.append(
            f"{', '.join(invalid_for_intent)} must use intent=CLARIFY"
        )
    return errors


def validate_proposal(
    proposal: object,
    *,
    schema: Mapping[str, Any] | None = None,
    capabilities: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    active_schema = dict(schema or load_json_object(default_schema_path()))
    active_capabilities = dict(
        capabilities or load_json_object(default_capabilities_path())
    )
    errors = _validate_structure(proposal, active_schema)
    if not errors and isinstance(proposal, dict):
        errors.extend(_validate_semantics(proposal, active_capabilities))
    if errors:
        raise ProposalValidationError(errors)
    return dict(proposal)


def validate_activity_context(
    context: Mapping[str, Any] | None,
    *,
    schema: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    normalized = default_activity_context()
    if context:
        normalized.update(dict(context))
    active_schema = dict(
        schema or load_json_object(default_activity_context_schema_path())
    )
    errors = _validate_structure(normalized, active_schema)
    if errors:
        raise ProposalValidationError(errors)
    return normalized


def safe_fallback(*, unavailable: bool, question: str | None = None) -> dict[str, Any]:
    return {
        "intent": "CLARIFY",
        "device": None,
        "action": None,
        "parameters": {},
        "ambiguous": True,
        "safety_flags": [
            "MODEL_UNAVAILABLE" if unavailable else "MODEL_OUTPUT_INVALID"
        ],
        "clarification_question": question
        or "요청을 안전하게 해석하지 못했습니다. 대상 기기와 원하는 동작을 다시 말해 주세요.",
    }
