"""PC rehearsal of the demo scenes against demo/pc/docker-compose.yml (simulation only).

Runs the voice parser and the LLM node in-process, sends demo sentences, approves AI/door/scenario
proposals like the UI would, and prints the task states core reports.

  .venv/Scripts/python tools/rehearse.py [--no-llm] [--approve]
"""

from __future__ import annotations

import argparse
import json
import threading
import time
from typing import Any

from homemind_runtime import pfn
from homemind_runtime.action_map import ActionMap
from homemind_runtime.config import BrokerConfig
from homemind_runtime.mqtt import Bus
from homemind_runtime.voice import VoiceParser

SCENES = [
    ("음성 명령", "거실 불 켜줘", None),
    ("음성 명령", "화장실 문 열어줘", None),
    ("음성 명령(승인)", "현관문 열어줘", None),
    ("반대 동작 버그 수정", "켜져 있는 티비 꺼줘", None),
    ("회의 중 개별 명령", "창문 닫아줘", {"presence_zone": "DESK", "activity": "MEETING", "audio_context": "ZOOM_SPEAKER", "quiet_required": True, "confidence": 0.9}),
    ("조용 모드(음성)", "조용히 해줘", {"presence_zone": "DESK", "activity": "MEETING", "audio_context": "ZOOM_SPEAKER", "quiet_required": True, "confidence": 0.9}),
    ("숨은 의도(LLM)", "아 좀 덥네", None),
    ("기존 시나리오(승인)", "귀가 준비해줘", None),
    ("안전", "에어컨 켜지 말라니까", None),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-llm", action="store_true")
    parser.add_argument("--approve", action="store_true", help="approve proposed tasks like the UI")
    args = parser.parse_args()

    broker = BrokerConfig()
    updates: dict[str, list[dict[str, Any]]] = {}
    llm_results: list[dict[str, Any]] = []
    lock = threading.Lock()
    bus = Bus(broker, "rehearsal-ui")

    def on_update(topic: str, doc: dict[str, Any]) -> None:
        del topic
        task = doc["task"]
        with lock:
            updates.setdefault(task["task_id"], []).append(task)
        if args.approve and task["state"] == "proposed":
            bus.publish("pfn/v1/ui/action", {
                "schema_version": "pfn.v1", "event_id": pfn.new_id(), "trace_id": task["trace_id"],
                "task_id": task["task_id"], "client_id": "rehearsal-ui", "action": "approve",
                "expected_revision": task["revision"], "action_token": task["action_token"],
                "reason": "rehearsal approval", "sent_at_ms": pfn.now_ms()})

    def on_llm_result(topic: str, doc: dict[str, Any]) -> None:
        del topic
        with lock:
            llm_results.append(doc)

    bus.subscribe("pfn/v1/core/task/update", on_update)
    bus.subscribe("pfn/v1/ai/llm/result", on_llm_result)
    bus.start()

    llm_bus = None
    if not args.no_llm:
        from homemind_exaone.client import ExaoneClient

        from homemind_runtime.llm_node import LlmNode

        node = LlmNode(ExaoneClient(model="exaone-1.2b-homemind-v3", timeout_seconds=30.0), ActionMap(), simulated=True)
        llm_bus = Bus(broker, "rehearsal-llm")
        llm_bus.subscribe("pfn/v1/ai/llm/request", lambda t, d: [llm_bus.publish(tp, m) for tp, m in node.handle(d)])
        llm_bus.start()

    voice = VoiceParser(ActionMap(), simulated=True)
    sent: list[tuple[str, str, str | None]] = []
    for scene, text, context in SCENES:
        decision = voice.handle(text, context=context)
        task_id = None
        for topic, message in decision.messages:
            bus.publish(topic, message)
            task_id = message.get("task_id", task_id)
        sent.append((scene, text, task_id or decision.kind))
        time.sleep(1.5)
    time.sleep(6)

    with lock:
        llm_tasks = {r["task_id"]: r for r in llm_results if r.get("task_id")}
        print(json.dumps([{"status": r["status"], "ai": [r["ai_intent"], r["ai_device"], r["ai_action"]],
                           "latency_ms": r["model"]["latency_ms"]} for r in llm_results], ensure_ascii=False))
        for task_id, history in updates.items():
            if task_id in llm_tasks:
                sent.append(("숨은 의도 → core", "(LLM 제안)", task_id))
        for scene, text, ref in sent:
            states = [t["state"] for t in updates.get(ref or "", [])]
            failure = next((t.get("failure", {}).get("code") for t in updates.get(ref or "", []) if t.get("failure")), None)
            print(f"{scene:16} | {text:14} | {ref if ref and ref not in updates else ''} {' > '.join(states)} {failure or ''}")
    bus.stop()
    if llm_bus:
        llm_bus.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
