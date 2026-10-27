"""Proposal expiry check for tests/run-pc-docker-test.sh (task-policy.json policy_version 3). Simulation only.

Core runs with a policy copy: default proposal_ttl_seconds 45, entrance door rule 30. This script sends an LLM
proposal (LED, waits 45 s) and a voice request to open the entrance door (waits 30 s), then checks in the database:
both wait as proposed; at ~35 s the door request is rejected PROPOSAL_EXPIRED while the LLM proposal still waits; at
~48 s the LLM proposal is expired too; a late approve with the old revision/token changes nothing and sends no command.
    python3 proposal-expiry-check.py <contracts dir> <database> <broker port>
"""
import json
import sqlite3
import subprocess
import sys
import time
import uuid
from pathlib import Path

contracts, database, port = Path(sys.argv[1]), sys.argv[2], sys.argv[3]


def publish(topic: str, document: dict) -> None:
    subprocess.run(["mosquitto_pub", "-h", "127.0.0.1", "-p", port, "-q", "1", "-t", topic, "-m",
                    json.dumps(document)], check=True)


def task(task_id: str) -> sqlite3.Row | None:
    with sqlite3.connect(database) as db:
        db.row_factory = sqlite3.Row
        return db.execute("SELECT state, rejection_code, revision, action_token FROM tasks WHERE task_id=?",
                          (task_id,)).fetchone()


def wait_for(description: str, condition, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.5)
    raise SystemExit(f"PROPOSAL_EXPIRY_FAIL {description}")


def request(example: str, device_id: str | None = None) -> str:
    document = json.loads((contracts / "examples" / example).read_text(encoding="utf-8"))
    document.update(event_id=str(uuid.uuid4()), trace_id=str(uuid.uuid4()), task_id=str(uuid.uuid4()),
                    requested_at_ms=int(time.time() * 1000))
    if device_id:
        document["target_device"] = document["parameters"]["target_device_id"] = device_id
        document["parameters"]["ai_device"] = "ENTRANCE_DOOR"
    publish("pfn/v1/core/task/request", document)
    return document["task_id"]


start = time.monotonic()
llm_task = request("task-request-device-control.json")   # origin llm -> llm_needs_approval (default TTL)
# The example opens the entrance door by voice: it waits for approval with its own rule TTL.
door_task = request("task-request-voice-door.json", "SERVO-DOOR-01")
wait_for("both requests wait for approval", lambda: all(
    (row := task(t)) is not None and row["state"] == "proposed" for t in (llm_task, door_task)), 5)
waiting_llm = task(llm_task)

wait_for("door request expires at its rule TTL (30 s)", lambda: task(door_task)["rejection_code"] == "PROPOSAL_EXPIRED",
         40 - (time.monotonic() - start))
if task(llm_task)["state"] != "proposed" or time.monotonic() - start > 44:
    raise SystemExit("PROPOSAL_EXPIRY_FAIL the LLM proposal must still wait until the default TTL (45 s)")
print("PROPOSAL_RULE_TTL_PASS door=rejected/PROPOSAL_EXPIRED llm=proposed "
      f"elapsed_s={time.monotonic() - start:.1f}")

wait_for("LLM proposal expires at the default TTL (45 s)",
         lambda: task(llm_task)["rejection_code"] == "PROPOSAL_EXPIRED", 55 - (time.monotonic() - start))
print(f"PROPOSAL_DEFAULT_TTL_PASS llm=rejected/PROPOSAL_EXPIRED elapsed_s={time.monotonic() - start:.1f}")

# A late approve (from a screen that still showed the old proposal) must not run anything.
publish("pfn/v1/ui/action", {
    "schema_version": "pfn.v1", "event_id": str(uuid.uuid4()), "trace_id": str(uuid.uuid4()), "task_id": llm_task,
    "client_id": "homemind-ui-7inch", "action": "approve", "expected_revision": waiting_llm["revision"],
    "action_token": waiting_llm["action_token"], "sent_at_ms": int(time.time() * 1000)})
time.sleep(2)
with sqlite3.connect(database) as db:
    feedback = db.execute("SELECT accepted FROM user_feedback WHERE task_id=?", (llm_task,)).fetchall()
    commands = db.execute("SELECT COUNT(*) FROM commands WHERE task_id IN (?, ?)", (llm_task, door_task)).fetchone()[0]
if task(llm_task)["state"] != "rejected" or feedback != [(0,)] or commands != 0:
    raise SystemExit(f"PROPOSAL_EXPIRY_FAIL late approve: state={task(llm_task)['state']} feedback={feedback} "
                     f"commands={commands}")
print("PROPOSAL_LATE_APPROVE_REFUSED_PASS")
