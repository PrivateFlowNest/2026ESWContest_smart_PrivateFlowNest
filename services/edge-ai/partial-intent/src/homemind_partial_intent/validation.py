"""Contract validation for emitted partial-intent events."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator, FormatChecker


WORKSPACE_ROOT = Path(__file__).resolve().parents[5]
SCHEMA_PATH = WORKSPACE_ROOT / "contracts" / "rpi5-internal" / "partial-intent.schema.json"


def validate_event(event: dict[str, Any]) -> None:
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    Draft202012Validator(schema, format_checker=FormatChecker()).validate(event)
