"""P4 pass criterion: PC E2E of the C++ RPI5 services, 10 rounds (SIMULATION — PC x86_64 Docker, not a Pi).

    PC Mosquitto + homemind-core 0.6.0 (x86) + fake ESP32/ESP8266   demo/pc docker compose --profile fake-esp
    whisper-server (x86, Whisper small LoRA v4 Q5_0)                 container, host port 127.0.0.1:8090
    llama-server (EXAONE v3 Q4_K_M)                                  Windows, 127.0.0.1:8080
    homemind-llm (C++)                                               container, long-running
    homemind-perception (C++)                                        container per scene, --wav real AIHub audio
    homemind-voice-text (C++)                                        container per round, typed sentences
    this test                                                        plays the 7-inch UI (approves proposals)

Per round: two real spoken commands -> voice parser -> core -> fake ESP ACK -> completed; a real meeting recording ->
context path -> ACTIVATED -> core quiet_mode_on -> completed, then a quiet room -> DEACTIVATED -> quiet_mode_off (one
activation per meeting, policy v6.3); "아 좀 덥네" -> llm/request -> EXAONE v3 -> proposal ->
approval -> completed with one trace_id end to end; "에어컨 켜지 말라니까" -> nothing. Every message is schema checked.

    HM_E2E_ROUNDS=10 services/runtime/.venv/Scripts/python -m pytest native/tests/test_e2e_pc_native.py -s
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import uuid
import wave
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import IMAGE, REPO, build, validate_topic_message  # noqa: E402

BUILDER = "e2e"
AIHUB = Path(os.environ.get("HM_AIHUB_DIR", "E:/HomeMind-Datasets/raw/aihub"))
MANIFEST = REPO / "services/edge-ai/speech-to-text/artifacts/manifests/aihub-71405-home-near-homemind-100.jsonl"
WHISPER_MODEL = "services/edge-ai/speech-to-text/artifacts/training/whisper-small-homemind-lora-v4/ggml-whisper-small-homemind-lora-v4-q5_0.bin"
# Real AIHub command clips (reference text -> expected registry device after the voice parser).
COMMANDS = {"텔레비전 틀어라.": "LED-TV-01", "너무 더우니까 에어컨 켜.": "LED-AIRCON-01"}
ROUNDS = int(os.environ.get("HM_E2E_ROUNDS", "10"))
TOPICS = ["pfn/v1/core/task/request", "pfn/v1/core/task/update", "pfn/v1/ai/llm/request", "pfn/v1/ai/llm/result",
          "pfn/v1/ai/perception"]


def _port_open(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def _llama_up() -> bool:
    try:
        return b"ok" in urllib.request.urlopen("http://127.0.0.1:8080/health", timeout=2).read()
    except OSError:
        return False


def _docker(*args: str, check: bool = True, timeout: float = 600, stdin: str | None = None):
    env = {**os.environ, "MSYS_NO_PATHCONV": "1"}
    return subprocess.run(["docker", *args], capture_output=True, text=True, encoding="utf-8", check=check,
                          timeout=timeout, input=stdin, env=env)


class Observer:
    """MQTT side of the test: records pfn.v1 traffic and approves proposals like the 7-inch UI."""

    def __init__(self) -> None:
        import paho.mqtt.client as mqtt

        self.lock = threading.Lock()
        self.messages: list[tuple[float, str, dict[str, Any]]] = []
        self.invalid: list[str] = []
        self.approve_traces: set[str] = set()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"e2e-ui-{uuid.uuid4().hex[:6]}")
        self.client.on_connect = lambda c, u, f, rc, p: [c.subscribe(t, qos=1) for t in TOPICS]
        self.client.on_message = self._on_message
        self.client.connect("127.0.0.1", 1883, 30)
        self.client.loop_start()
        time.sleep(1.0)

    def _on_message(self, client, userdata, message) -> None:
        document = json.loads(message.payload.decode("utf-8"))
        if message.topic != "pfn/v1/core/task/update":
            try:
                validate_topic_message(message.topic, document)
            except AssertionError as error:
                self.invalid.append(f"{message.topic}: {error}")
        with self.lock:
            self.messages.append((time.time(), message.topic, document))
        if message.topic == "pfn/v1/core/task/update":
            task = document["task"]
            if task["state"] == "proposed" and document["trace_id"] in self.approve_traces:
                self.client.publish("pfn/v1/ui/action", json.dumps({
                    "schema_version": "pfn.v1", "event_id": str(uuid.uuid4()), "trace_id": document["trace_id"],
                    "task_id": task["task_id"], "client_id": "e2e-ui", "action": "approve",
                    "expected_revision": task["revision"], "action_token": task["action_token"],
                    "reason": "E2E approval (simulated UI)", "sent_at_ms": int(time.time() * 1000)}), qos=1)

    def since(self, start: float, topic: str | None = None) -> list[dict[str, Any]]:
        with self.lock:
            return [doc for at, t, doc in self.messages if at >= start and (topic is None or t == topic)]

    def wait(self, predicate, timeout: float) -> Any:
        deadline = time.time() + timeout
        while time.time() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.2)
        return predicate()

    def task_states(self, task_id: str) -> list[str]:
        with self.lock:
            return [doc["task"]["state"] for _, t, doc in self.messages
                    if t == "pfn/v1/core/task/update" and doc["task"]["task_id"] == task_id]

    def stop(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()


@pytest.fixture(scope="module")
def stack(tmp_path_factory):
    if not _port_open(1883):
        pytest.skip("PC demo broker not running (docker compose -f demo/pc/docker-compose.yml --profile fake-esp up -d)")
    if not _llama_up():
        pytest.skip("llama-server (EXAONE v3) not running on 127.0.0.1:8080")
    if not MANIFEST.exists() or not AIHUB.is_dir():
        pytest.skip("AIHub clips not available")
    build(BUILDER, build_type="Release", target="homemind-perception homemind-llm homemind-voice-text")
    work = tmp_path_factory.mktemp("e2e")
    # Core keeps the last context-rule intent per group in memory ("same intent not twice in a row"); a previous
    # run that ended in a meeting would swallow this run's first quiet_mode_on, so start from a fresh core.
    restarted = time.time()
    _docker("restart", "homemind-pc-demo-core-1")
    while "CORE_STARTED" not in _docker("logs", "--since", str(int(restarted) - 1), "homemind-pc-demo-core-1",
                                        check=False).stdout and time.time() < restarted + 30:
        time.sleep(0.5)
    # scenes: two command clips + a 75 s meeting segment (same segment agent B's parity test uses) followed by 25 s
    # of quiet room, so the context path also releases (policy v6.3 holds quiet for 15 updates, ~18 s)
    rows = [json.loads(line) for line in MANIFEST.read_text(encoding="utf-8").splitlines()]
    clips = {}
    for row in rows:
        if row["reference"] in COMMANDS and row["reference"] not in clips:
            target = work / f"cmd{len(clips)}.wav"
            target.write_bytes(Path(row["audio_path"]).read_bytes())
            clips[row["reference"]] = target
    assert len(clips) == len(COMMANDS), "command clips missing from the manifest"
    import glob
    import itertools

    meeting_source = next((Path(p) for p in itertools.islice(glob.iglob(str(AIHUB / "464_meeting/**/*.wav"),
                                                                       recursive=True), 1)), None)
    assert meeting_source, "AIHub meeting audio missing"
    meeting = work / "meeting.wav"
    with wave.open(str(meeting_source), "rb") as reader, wave.open(str(meeting), "wb") as writer:
        reader.setpos(int(900.0 * reader.getframerate()))
        writer.setparams(reader.getparams())
        writer.writeframes(reader.readframes(int(75.0 * reader.getframerate())))
        quiet_room = np.random.default_rng(20261022).normal(0, 30, int(25.0 * reader.getframerate()))
        writer.writeframes(quiet_room.astype("<i2").tobytes())
    common = {"broker": {"host": "host.docker.internal", "port": 1883}, "contracts_dir": "/w/contracts",
              "simulated": True, "llm_base_url": "http://host.docker.internal:8080",
              "llm_model": "exaone-1.2b-homemind-v3", "base_dir": "/w"}
    (work / "llm.json").write_text(json.dumps({**common, "capabilities_path": "services/exaone-edge/config/device-capabilities.json"}), "utf-8")
    (work / "perception.json").write_text(json.dumps({
        **common, "whisper_server_url": "http://host.docker.internal:8090", "listen_mode": "off",
        "device_capabilities": "services/exaone-edge/config/device-capabilities.json"}), "utf-8")
    (work / "voice-text.json").write_text(json.dumps({**common, "capabilities_path": "services/exaone-edge/config/device-capabilities.json"}), "utf-8")
    name = f"hm-e2e-{uuid.uuid4().hex[:6]}"
    mounts = ["-v", f"{REPO.as_posix()}:/w", "-v", f"hm-native-build-{BUILDER}:/build", "-v", f"{work.as_posix()}:/t"]
    _docker("run", "-d", "--rm", "--name", f"{name}-whisper", "-p", "127.0.0.1:8090:8090", *mounts, IMAGE,
            "/w/native/.deps/x64-tools/bin/whisper-server", "-m", f"/w/{WHISPER_MODEL}", "--host", "0.0.0.0",
            "--port", "8090", "-t", "8", "-l", "ko")
    _docker("run", "-d", "--rm", "--name", f"{name}-llm", *mounts, IMAGE,
            "/build/services/homemind-llm/homemind-llm", "--config", "/t/llm.json")
    deadline = time.time() + 60
    while not _port_open(8090) and time.time() < deadline:
        time.sleep(1)
    time.sleep(3)
    observer = Observer()
    yield {"work": work, "clips": clips, "meeting": meeting, "mounts": mounts, "observer": observer, "name": name}
    observer.stop()
    logs = _docker("logs", f"{name}-llm", check=False).stderr
    (work / "llm.log").write_text(logs, "utf-8")
    for suffix in ("whisper", "llm"):
        _docker("rm", "-f", f"{name}-{suffix}", check=False)


def _perception(stack, wav: Path, *extra: str) -> subprocess.CompletedProcess:
    return _docker("run", "--rm", *stack["mounts"], IMAGE, "/build/services/homemind-perception/homemind-perception",
                   "--config", "/t/perception.json", "--wav", f"/t/{wav.name}", "--fast", *extra,
                   check=False, timeout=600)


def _voice_text(stack, lines: list[str]) -> subprocess.CompletedProcess:
    return _docker("run", "--rm", "-i", *stack["mounts"], IMAGE, "/build/services/homemind-voice-text/homemind-voice-text",
                   "--config", "/t/voice-text.json", stdin="\n".join(lines) + "\n", check=False, timeout=120)


def _completed(observer: Observer, task_id: str, timeout: float = 15) -> list[str]:
    return observer.wait(lambda: (s := observer.task_states(task_id)) and s[-1] in {"completed", "failed", "rejected"}
                         and s, timeout) or observer.task_states(task_id)


def test_pc_e2e_native_rounds(stack):
    observer: Observer = stack["observer"]
    results: list[dict[str, Any]] = []
    for round_index in range(ROUNDS):
        record: dict[str, Any] = {"round": round_index + 1}
        # 1) real spoken commands -> voice parser -> core -> fake ESP ACK
        for text, device in COMMANDS.items():
            start = time.time()
            run = _perception(stack, stack["clips"][text], "--voice", "whole")
            assert run.returncode == 0, run.stderr[-2000:]
            request = observer.wait(lambda: next((d for d in observer.since(start, "pfn/v1/core/task/request")
                                                  if d["target_device"] == device), None), 20)
            assert request, f"round {round_index + 1}: no task request for {text!r}\n{run.stderr[-1500:]}"
            assert request["parameters"]["origin"] == "voice_parser" and request["source_node_id"] == "NODE-RPI5-02"
            states = _completed(observer, request["task_id"])
            update_traces = {d["trace_id"] for d in observer.since(start, "pfn/v1/core/task/update")
                             if d["task"]["task_id"] == request["task_id"]}
            assert states and states[-1] == "completed", (text, states)
            assert update_traces == {request["trace_id"]}
            record[device] = " > ".join(states)
        # 2) real meeting audio -> context path -> ACTIVATED (quiet) -> core quiet_mode_on, then the quiet room tail
        #    -> DEACTIVATED -> core quiet_mode_off (context rule); one activation for the whole meeting (no flapping)
        start = time.time()
        run = _perception(stack, stack["meeting"], "--voice", "off")
        assert run.returncode == 0, run.stderr[-2000:]
        perceptions = observer.wait(lambda: [d for d in observer.since(start, "pfn/v1/ai/perception")
                                             if d["attributes"].get("transition") == "ACTIVATED"
                                             and d["attributes"].get("quiet_required")], 10)
        assert perceptions, "no quiet ACTIVATED perception from the meeting clip"
        quiet = observer.wait(lambda: next((d["task"] for d in observer.since(start, "pfn/v1/core/task/update")
                                            if d["task"]["intent"] == "quiet_mode_on"
                                            and d["task"]["state"] == "completed"), None), 20)
        assert quiet, "core did not complete quiet_mode_on"
        record["quiet_mode_on"] = "completed"
        released = observer.wait(lambda: next((d["task"] for d in observer.since(start, "pfn/v1/core/task/update")
                                               if d["task"]["intent"] == "quiet_mode_off"
                                               and d["task"]["state"] == "completed"), None), 20)
        assert released, "core did not complete quiet_mode_off after the meeting audio stopped"
        transitions = [d["attributes"].get("transition") for d in observer.since(start, "pfn/v1/ai/perception")]
        assert transitions.count("ACTIVATED") == 1 and transitions.count("DEACTIVATED") == 1, transitions
        record["quiet_release"] = "completed (1 activation, 1 release)"
        # 3) hidden intent -> EXAONE v3 (C++ LLM node) -> proposal -> UI approval -> completed, one trace id
        start = time.time()
        llm_trace: dict[str, str] = {}

        def find_llm_request():
            found = next((d for d in observer.since(start, "pfn/v1/ai/llm/request")), None)
            if found:
                observer.approve_traces.add(found["trace_id"])
                llm_trace["id"] = found["trace_id"]
            return found

        run = _voice_text(stack, ["아 좀 덥네"])
        assert run.returncode == 0, run.stderr[-2000:]
        assert observer.wait(find_llm_request, 10), "no llm/request"
        result = observer.wait(lambda: next((d for d in observer.since(start, "pfn/v1/ai/llm/result")
                                             if d["trace_id"] == llm_trace["id"]), None), 40)
        assert result and result["status"] == "task_requested", result
        task = observer.wait(lambda: next((d for d in observer.since(start, "pfn/v1/core/task/request")
                                           if d["trace_id"] == llm_trace["id"]), None), 10)
        assert task and task["parameters"]["origin"] == "llm" and task["task_id"] == result["task_id"]
        states = _completed(observer, task["task_id"], 25)
        assert "proposed" in states and states[-1] == "completed", states
        record["llm"] = f"{result['ai_device']}/{result['ai_action']} {result['model']['latency_ms']:.0f} ms: " + " > ".join(states)
        # 4) negation -> nothing at all
        start = time.time()
        run = _voice_text(stack, ["에어컨 켜지 말라니까"])
        assert run.returncode == 0, run.stderr[-2000:]
        time.sleep(3)
        assert not observer.since(start, "pfn/v1/core/task/request")
        assert not observer.since(start, "pfn/v1/ai/llm/request")
        record["negation"] = "no message"
        # 5) "회의 끝났어" -> quiet_mode_off (voice parser scenario, runs at once, not deduplicated like context rules)
        start = time.time()
        run = _voice_text(stack, ["회의 끝났어"])
        assert run.returncode == 0, run.stderr[-2000:]
        off = observer.wait(lambda: next((d for d in observer.since(start, "pfn/v1/core/task/request")
                                          if d["intent"] == "quiet_mode_off"), None), 10)
        assert off, "no quiet_mode_off request"
        states = _completed(observer, off["task_id"])
        assert states and states[-1] == "completed", states
        record["quiet_mode_off"] = " > ".join(states)
        results.append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)
    assert not observer.invalid, observer.invalid[:5]
    report = REPO / "native" / "tests" / "reports"
    report.mkdir(exist_ok=True)
    (report / "e2e-pc-native.json").write_text(json.dumps({
        "real_hardware_executed": False, "environment": "PC x86_64 Docker + Windows llama-server, fake ESP",
        "rounds": results, "schema_invalid": observer.invalid}, ensure_ascii=False, indent=1), "utf-8")
    assert len(results) == ROUNDS
    logs = _docker("logs", f"{stack['name']}-llm", check=False)
    assert "아 좀 덥네" not in logs.stdout + logs.stderr, "transcript in homemind-llm logs"
