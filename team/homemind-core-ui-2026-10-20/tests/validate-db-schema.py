#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sqlite3


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_embedded_schema() -> str:
    source = (ROOT / "src" / "schema.hpp").read_text(encoding="utf-8")
    match = re.search(r'R"SQL\(\s*(.*?)\s*\)SQL";', source, re.DOTALL)
    if not match:
        raise RuntimeError("embedded schema SQL was not found")
    return match.group(1)


def validate(sql: str, label: str) -> None:
    database = sqlite3.connect(":memory:")
    database.executescript(sql)
    required_columns = {
        "commands": {"target_node_id", "target_device_id"},
        "acknowledgements": {"node_id", "target_device_id"},
        "tasks": {"revision", "action_token", "rejection_code", "rejection_message"},
    }
    for table, expected in required_columns.items():
        actual = {row[1] for row in database.execute(f"PRAGMA table_info({table})")}
        missing = expected - actual
        if missing:
            raise RuntimeError(f"{label}: {table} missing columns {sorted(missing)}")
    required_tables = {
        "safety_alerts",
        "safety_acknowledgements",
        "privacy_events",
        "privacy_current",
        "feedback_events",
        "system_health_reports",
    }
    actual_tables = {
        row[0]
        for row in database.execute("SELECT name FROM sqlite_master WHERE type='table'")
    }
    missing_tables = required_tables - actual_tables
    if missing_tables:
        raise RuntimeError(f"{label}: missing tables {sorted(missing_tables)}")
    version = database.execute(
        "SELECT value FROM metadata WHERE key='schema_version'"
    ).fetchone()
    if version != ("5",):
        raise RuntimeError(f"{label}: unexpected schema version {version}")


def main() -> None:
    validate((ROOT / "sql" / "schema.sql").read_text(encoding="utf-8"), "sql/schema.sql")
    validate(load_embedded_schema(), "src/schema.hpp")
    print("DB_SCHEMA_VALIDATION_PASS version=5")


if __name__ == "__main__":
    main()
