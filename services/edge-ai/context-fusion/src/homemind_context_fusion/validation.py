"""Contract validation helpers."""

from __future__ import annotations

import json
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker


REPOSITORY_ROOT = Path(__file__).resolve().parents[5]
EVENT_SCHEMA = REPOSITORY_ROOT / "contracts" / "rpi5-internal" / "context-activity-event.schema.json"
FEATURE_EVENT_SCHEMA = (
    REPOSITORY_ROOT / "contracts" / "rpi5-internal" / "context-feature-observation.schema.json"
)
SANITIZED_SCHEMA = REPOSITORY_ROOT / "contracts" / "rpi5-internal" / "activity-context.schema.json"


def _validate(value: object, path: Path) -> None:
    schema = json.loads(path.read_text(encoding="utf-8"))
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(value)


def validate_event(value: object) -> None:
    _validate(value, EVENT_SCHEMA)


def validate_feature_event(value: object) -> None:
    _validate(value, FEATURE_EVENT_SCHEMA)


def validate_sanitized_context(value: object) -> None:
    _validate(value, SANITIZED_SCHEMA)
