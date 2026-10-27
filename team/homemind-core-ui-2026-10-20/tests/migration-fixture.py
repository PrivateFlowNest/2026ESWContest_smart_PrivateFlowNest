#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sqlite3
import sys


LEGACY_COMMAND_ID = "00000000-0000-4000-8000-000000000101"
LEGACY_TASK_ID = "00000000-0000-4000-8000-000000000102"


def create(database_path: pathlib.Path) -> None:
    root = pathlib.Path(__file__).resolve().parents[1]
    database = sqlite3.connect(database_path)
    database.executescript((root / "tests" / "legacy-schema.sql").read_text(encoding="utf-8"))
    database.executescript(
        """
        CREATE TABLE commands (
          command_id TEXT PRIMARY KEY, event_id TEXT NOT NULL, task_id TEXT,
          trace_id TEXT NOT NULL, target_node_id TEXT NOT NULL, action TEXT NOT NULL,
          parameters_json TEXT NOT NULL, issued_at_ms INTEGER NOT NULL,
          expires_at_ms INTEGER NOT NULL, latest_attempt INTEGER NOT NULL,
          status TEXT NOT NULL, first_received_at_ms INTEGER NOT NULL,
          updated_at_ms INTEGER NOT NULL
        );
        CREATE TABLE acknowledgements (
          event_id TEXT PRIMARY KEY, command_id TEXT NOT NULL, trace_id TEXT NOT NULL,
          node_id TEXT NOT NULL, ok INTEGER NOT NULL, duplicate INTEGER NOT NULL,
          status TEXT NOT NULL, executed_at_ms INTEGER NOT NULL, result_json TEXT,
          error_json TEXT, received_at_ms INTEGER NOT NULL
        );
        CREATE TABLE tasks (
          task_id TEXT PRIMARY KEY, request_event_id TEXT NOT NULL UNIQUE,
          trace_id TEXT NOT NULL, source_node_id TEXT NOT NULL, intent TEXT NOT NULL,
          target_device TEXT NOT NULL, priority INTEGER NOT NULL,
          safety_level TEXT NOT NULL, state TEXT NOT NULL, interruptible INTEGER NOT NULL,
          resume_policy TEXT NOT NULL, confidence REAL NOT NULL,
          parameters_json TEXT NOT NULL, requested_at_ms INTEGER NOT NULL,
          created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL,
          revision INTEGER NOT NULL DEFAULT 1
        );
        """
    )
    trace_id = "00000000-0000-4000-8000-000000000103"
    database.execute(
        "INSERT INTO commands VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            LEGACY_COMMAND_ID,
            "00000000-0000-4000-8000-000000000104",
            None,
            trace_id,
            "NODE-ESP32-LEGACY",
            "set_led",
            '{"on":true}',
            1,
            2,
            1,
            "succeeded",
            1,
            1,
        ),
    )
    database.execute(
        "INSERT INTO acknowledgements VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        (
            "00000000-0000-4000-8000-000000000105",
            LEGACY_COMMAND_ID,
            trace_id,
            "NODE-ESP32-LEGACY",
            1,
            0,
            "succeeded",
            1,
            None,
            None,
            1,
        ),
    )
    database.execute(
        "INSERT INTO tasks VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            LEGACY_TASK_ID,
            "00000000-0000-4000-8000-000000000106",
            trace_id,
            "NODE-RPI5-LEGACY",
            "legacy_test",
            "DEV-LEGACY-01",
            10,
            "normal",
            "completed",
            1,
            "never",
            1.0,
            "{}",
            1,
            1,
            1,
            1,
        ),
    )
    database.commit()


def verify(database_path: pathlib.Path) -> None:
    database = sqlite3.connect(database_path)
    command = database.execute(
        "SELECT target_node_id,target_device_id FROM commands WHERE command_id=?",
        (LEGACY_COMMAND_ID,),
    ).fetchone()
    acknowledgement = database.execute(
        "SELECT node_id,target_device_id FROM acknowledgements WHERE command_id=?",
        (LEGACY_COMMAND_ID,),
    ).fetchone()
    task = database.execute(
        "SELECT length(action_token) FROM tasks WHERE task_id=?", (LEGACY_TASK_ID,)
    ).fetchone()
    version = database.execute(
        "SELECT value FROM metadata WHERE key='schema_version'"
    ).fetchone()
    if command != ("NODE-ESP32-LEGACY", "NODE-ESP32-LEGACY"):
        raise RuntimeError(f"legacy command migration failed: {command}")
    if acknowledgement != ("NODE-ESP32-LEGACY", "NODE-ESP32-LEGACY"):
        raise RuntimeError(f"legacy acknowledgement migration failed: {acknowledgement}")
    if not task or task[0] < 16:
        raise RuntimeError(f"legacy task token migration failed: {task}")
    task_columns = {row[1] for row in database.execute("PRAGMA table_info(tasks)")}
    if not {"rejection_code", "rejection_message"} <= task_columns:
        raise RuntimeError(f"legacy task rejection columns missing: {sorted(task_columns)}")
    if version != ("5",):
        raise RuntimeError(f"legacy schema version migration failed: {version}")
    print("LEGACY_MIGRATION_PASS version=5")


def main() -> None:
    if len(sys.argv) != 3 or sys.argv[1] not in {"create", "verify"}:
        raise SystemExit("usage: migration-fixture.py create|verify DATABASE")
    database_path = pathlib.Path(sys.argv[2])
    if sys.argv[1] == "create":
        create(database_path)
    else:
        verify(database_path)


if __name__ == "__main__":
    main()
