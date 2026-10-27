"""30-minute PC soak + fault injection of the C++ RPI5 services (SIMULATION: PC x86_64 Docker, not a Pi).

Stack: demo/pc docker compose (Mosquitto + homemind-core 0.6 x86 + fake ESP32/ESP8266), Windows llama-server
(EXAONE v3, 127.0.0.1:8080), homemind-llm and homemind-perception (C++) in containers. The perception node plays
a 30-minute real-time audio stream built from AIHub meeting audio (6 min meeting / 2 min quiet room, repeated);
the voice path is driven through homemind-voice-text. This script plays the 7-inch UI (approves proposals).

Schedule (minutes): every 1: "거실 불 켜줘"/"거실 불 꺼줘" alternately; every 5: "아 좀 덥네" (EXAONE -> approve);
10: broker restart; 15-16.5: llama-server stopped ("아 좀 덥네" during the outage must give model_unavailable);
20: core restart. Memory of the C++ containers is sampled every 30 s.

    services/runtime/.venv/Scripts/python native/tests/soak_pc_native.py [--minutes 30]
Writes native/tests/reports/soak-pc-native.json and exits non-zero when a check fails.
"""

from __future__ import annotations

import argparse
import glob
import itertools
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid
import wave
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import IMAGE, REPO, build, validate_topic_message  # noqa: E402

BUILDER = "e2e"
LLAMA = Path(os.environ.get("HM_LLAMA_SERVER", "C:/Users/alexi/AppData/Local/Microsoft/WinGet/Packages/"
                            "ggml.llamacpp_Microsoft.Winget.Source_8wekyb3d8bbwe/llama-server.exe"))
GGUF = REPO / "services/exaone-edge/artifacts/exaone-1.2b-homemind-v3-q4_k_m.gguf"
AIHUB = Path(os.environ.get("HM_AIHUB_DIR", "E:/HomeMind-Datasets/raw/aihub"))
TOPICS = ["pfn/v1/core/task/request", "pfn/v1/core/task/update", "pfn/v1/ai/llm/request", "pfn/v1/ai/llm/result",
          "pfn/v1/ai/perception"]
ENV = {**os.environ, "MSYS_NO_PATHCONV": "1"}


def docker(*args: str, check: bool = True, timeout: float = 600, stdin: str | None = None):
    return subprocess.run(["docker", *args], capture_output=True, text=True, encoding="utf-8", check=check,
                          timeout=timeout, input=stdin, env=ENV)


def host_path(path: Path) -> str:
    return path.as_posix()


class Llama:
    def __init__(self) -> None:
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        self.proc = subprocess.Popen([str(LLAMA), "-m", str(GGUF), "--alias", "exaone-1.2b-homemind-v3", "--host",
                                      "127.0.0.1", "--port", "8080", "-c", "4096"], stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        for _ in range(120):
            try:
                if b"ok" in urllib.request.urlopen("http://127.0.0.1:8080/health", timeout=2).read():
                    return
            except OSError:
                pass
            time.sleep(1)
        raise RuntimeError("llama-server did not start")

    def stop(self) -> None:
        if self.proc:
            self.proc.terminate()
            self.proc.wait(timeout=30)
            self.proc = None


class Observer:
    def __init__(self) -> None:
        import paho.mqtt.client as mqtt

        self.lock = threading.Lock()
        self.messages: list[tuple[float, str, dict[str, Any]]] = []
        self.invalid: list[str] = []
        self.approve_traces: set[str] = set()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"soak-ui-{uuid.uuid4().hex[:6]}")
        self.client.on_connect = lambda c, u, f, rc, p: [c.subscribe(t, qos=1) for t in TOPICS]
        self.client.on_message = self._on_message
        self.client.reconnect_delay_set(1, 5)
        self.client.connect("127.0.0.1", 1883, 30)
        self.client.loop_start()
        time.sleep(1)

    def _on_message(self, client, userdata, message) -> None:
        document = json.loads(message.payload.decode("utf-8"))
        if message.topic != "pfn/v1/core/task/update":
            try:
                validate_topic_message(message.topic, document)
            except AssertionError as error:
                self.invalid.append(f"{message.topic}: {error}")
        with self.lock:
            self.messages.append((time.time(), message.topic, document))
        if message.topic == "pfn/v1/ai/llm/request":
            self.approve_traces.add(document["trace_id"])
        if message.topic == "pfn/v1/core/task/update":
            task = document["task"]
            if task["state"] == "proposed" and document["trace_id"] in self.approve_traces:
                client.publish("pfn/v1/ui/action", json.dumps({
                    "schema_version": "pfn.v1", "event_id": str(uuid.uuid4()), "trace_id": document["trace_id"],
                    "task_id": task["task_id"], "client_id": "soak-ui", "action": "approve",
                    "expected_revision": task["revision"], "action_token": task["action_token"],
                    "reason": "soak approval (simulated UI)", "sent_at_ms": int(time.time() * 1000)}), qos=1)

    def since(self, start: float, topic: str) -> list[dict[str, Any]]:
        with self.lock:
            return [doc for at, t, doc in self.messages if at >= start and t == topic]

    def task_final(self, task_id: str) -> str | None:
        with self.lock:
            states = [doc["task"]["state"] for _, t, doc in self.messages
                      if t == "pfn/v1/core/task/update" and doc["task"]["task_id"] == task_id]
        return states[-1] if states else None


def soak_audio(path: Path, minutes: float) -> None:
    source = next(Path(p) for p in itertools.islice(glob.iglob(str(AIHUB / "464_meeting/**/*.wav"), recursive=True), 1))
    with wave.open(str(source), "rb") as reader:
        rate = reader.getframerate()
        meeting = np.frombuffer(reader.readframes(reader.getnframes()), dtype="<i2")
    rng = np.random.default_rng(20261022)
    out, cursor, total = [], 0, int(minutes * 60 * rate)
    produced = 0
    while produced < total:
        chunk = meeting[cursor: cursor + 6 * 60 * rate]
        cursor = (cursor + len(chunk)) % (len(meeting) - 6 * 60 * rate)
        quiet = (rng.normal(0, 30, 2 * 60 * rate)).astype("<i2")  # quiet room, about -60 dBFS
        out += [chunk, quiet]
        produced += len(chunk) + len(quiet)
    audio = np.concatenate(out)[:total]
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(rate)
        writer.writeframes(audio.tobytes())


def mem_mib(container: str) -> float | None:
    result = docker("stats", "--no-stream", "--format", "{{.MemUsage}}", container, check=False, timeout=60)
    text = result.stdout.strip().split("/")[0].strip()
    for unit, scale in (("GiB", 1024), ("MiB", 1), ("KiB", 1 / 1024)):
        if text.endswith(unit):
            return round(float(text[: -len(unit)]) * scale, 1)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=float, default=30.0)
    args = parser.parse_args()
    minutes = args.minutes
    build(BUILDER, build_type="Release", target="homemind-perception homemind-llm homemind-voice-text")
    work = Path(tempfile.mkdtemp(prefix="hmsoak-"))
    soak_audio(work / "soak.wav", minutes + 3)  # outlives the run
    common = {"broker": {"host": "host.docker.internal", "port": 1883}, "contracts_dir": "/w/contracts",
              "simulated": True, "base_dir": "/w", "llm_base_url": "http://host.docker.internal:8080",
              "llm_model": "exaone-1.2b-homemind-v3"}
    caps = "services/exaone-edge/config/device-capabilities.json"
    (work / "llm.json").write_text(json.dumps({**common, "capabilities_path": caps}), "utf-8")
    (work / "voice-text.json").write_text(json.dumps({**common, "capabilities_path": caps}), "utf-8")
    (work / "perception.json").write_text(json.dumps({**common, "listen_mode": "off", "device_capabilities": caps}),
                                          "utf-8")
    mounts = ["-v", f"{host_path(REPO)}:/w", "-v", f"hm-native-build-{BUILDER}:/build", "-v", f"{host_path(work)}:/t"]
    llama = Llama()
    llama.start()
    for name in ("hm-soak-llm", "hm-soak-perception"):
        docker("rm", "-f", name, check=False)
    docker("run", "-d", "--name", "hm-soak-llm", *mounts, IMAGE, "/build/services/homemind-llm/homemind-llm",
           "--config", "/t/llm.json")
    docker("run", "-d", "--name", "hm-soak-perception", *mounts, IMAGE,
           "/build/services/homemind-perception/homemind-perception", "--config", "/t/perception.json",
           "--wav", "/t/soak.wav", "--voice", "off")
    observer = Observer()
    start = time.time()
    commands: list[dict[str, Any]] = []
    hidden: list[dict[str, Any]] = []
    memory: list[dict[str, Any]] = []
    events: list[str] = []
    done = {"broker": False, "llama_off": False, "llama_on": False, "core": False}
    last_cmd = last_hidden = last_mem = -999.0
    flip = True
    try:
        while (elapsed := (time.time() - start) / 60) < minutes:
            if elapsed - last_mem >= 0.5:
                memory.append({"min": round(elapsed, 1), "llm": mem_mib("hm-soak-llm"),
                               "perception": mem_mib("hm-soak-perception"),
                               "core": mem_mib("homemind-pc-demo-core-1")})
                last_mem = elapsed
            if elapsed >= minutes * 10 / 30 and not done["broker"]:
                docker("restart", "homemind-pc-demo-broker-1")
                done["broker"] = True
                events.append(f"{elapsed:.1f} broker restarted")
            if elapsed >= minutes * 15 / 30 and not done["llama_off"]:
                llama.stop()
                done["llama_off"] = True
                events.append(f"{elapsed:.1f} llama-server stopped")
            if elapsed >= minutes * 16.5 / 30 and not done["llama_on"]:
                llama.start()
                done["llama_on"] = True
                events.append(f"{elapsed:.1f} llama-server started")
            if elapsed >= minutes * 20 / 30 and not done["core"]:
                docker("restart", "homemind-pc-demo-core-1")
                done["core"] = True
                events.append(f"{elapsed:.1f} core restarted")
            if elapsed - last_cmd >= 1.0:
                text = "거실 불 켜줘" if flip else "거실 불 꺼줘"
                flip = not flip
                sent = time.time()
                docker("run", "--rm", "-i", *mounts, IMAGE, "/build/services/homemind-voice-text/homemind-voice-text",
                       "--config", "/t/voice-text.json", stdin=text + "\n", check=False, timeout=120)
                commands.append({"min": round(elapsed, 2), "text": text, "sent": sent})
                last_cmd = elapsed
            if elapsed - last_hidden >= 5.0 or (done["llama_off"] and not done["llama_on"]
                                                 and not any(h.get("outage") for h in hidden)):
                sent = time.time()
                docker("run", "--rm", "-i", *mounts, IMAGE, "/build/services/homemind-voice-text/homemind-voice-text",
                       "--config", "/t/voice-text.json", stdin="아 좀 덥네\n", check=False, timeout=120)
                hidden.append({"min": round(elapsed, 2), "sent": sent,
                               "outage": done["llama_off"] and not done["llama_on"]})
                last_hidden = elapsed
            time.sleep(2)
        running = {name: docker("inspect", "-f", "{{.State.Running}}", name, check=False).stdout.strip()
                   for name in ("hm-soak-llm", "hm-soak-perception")}
        time.sleep(30)  # let the last tasks finish
        # --- evaluation ---
        def near_restart(t: float) -> bool:
            return any(abs((t - start) / 60 - float(e.split()[0])) < 0.25 for e in events if "restarted" in e)

        voice_results = []
        for cmd in commands:
            requests = [d for d in observer.since(cmd["sent"] - 1, "pfn/v1/core/task/request")
                        if d["target_device"] == "LED-LIVING-01" and d["requested_at_ms"] / 1000 <= cmd["sent"] + 30]
            final = observer.task_final(requests[0]["task_id"]) if requests else None
            voice_results.append({"min": cmd["min"], "final": final, "near_fault": near_restart(cmd["sent"])})
        hidden_results = []
        for item in hidden:
            results = [d for d in observer.since(item["sent"] - 1, "pfn/v1/ai/llm/result")
                       if d["completed_at_ms"] / 1000 <= item["sent"] + 90]
            status = results[0]["status"] if results else None
            final = observer.task_final(results[0]["task_id"]) if results and results[0].get("task_id") else None
            hidden_results.append({"min": item["min"], "outage": item["outage"], "status": status, "final": final,
                                   "near_fault": near_restart(item["sent"])})
        transitions = [d["attributes"].get("transition") for d in observer.since(start, "pfn/v1/ai/perception")]
        quiet_tasks = {}
        for d in observer.since(start, "pfn/v1/core/task/update"):
            if d["task"]["intent"] in ("quiet_mode_on", "quiet_mode_off") and d["task"]["state"] == "completed":
                quiet_tasks[d["task"]["intent"]] = quiet_tasks.get(d["task"]["intent"], 0) + 1
        logs = docker("logs", "hm-soak-llm", check=False)
        warmup = min(5.0, minutes / 3)
        warm = [m for m in memory if m["min"] >= warmup and m["llm"] and m["perception"]]
        growth = {key: round(warm[-1][key] - warm[0][key], 1) for key in ("llm", "perception")} if len(warm) > 1 else {}
        checks = {
            "voice commands completed (outside fault windows)":
                all(r["final"] == "completed" for r in voice_results if not r["near_fault"]),
            "hidden intent completed when the model is up":
                all(r["final"] == "completed" for r in hidden_results if not r["outage"] and not r["near_fault"]),
            "model outage reported as model_unavailable":
                any(r["outage"] and r["status"] == "model_unavailable" for r in hidden_results),
            "context transitions both ways":
                transitions.count("ACTIVATED") >= 2 and transitions.count("DEACTIVATED") >= 2,
            "core quiet mode on/off completed": quiet_tasks.get("quiet_mode_on", 0) >= 2
                and quiet_tasks.get("quiet_mode_off", 0) >= 1,
            "services still running": all(v == "true" for v in running.values()),
            "memory growth after warm-up < 20 MiB": bool(growth) and all(v < 20 for v in growth.values()),
            "all messages schema-valid": not observer.invalid,
            "no transcript in llm logs": "아 좀 덥네" not in logs.stdout + logs.stderr,
        }
        report = {"real_hardware_executed": False, "environment": "PC x86_64 Docker + Windows llama-server, fake ESP",
                  "minutes": minutes, "events": events, "checks": checks, "memory_growth_mib": growth,
                  "memory": memory, "voice": voice_results, "hidden_intent": hidden_results,
                  "transitions": {k: transitions.count(k) for k in set(transitions) if k},
                  "quiet_tasks_completed": quiet_tasks, "schema_invalid": observer.invalid[:10]}
        out = REPO / "native/tests/reports/soak-pc-native.json"
        out.write_text(json.dumps(report, ensure_ascii=False, indent=1), "utf-8")
        print(json.dumps({"checks": checks, "growth": growth, "events": events}, ensure_ascii=False, indent=1))
        return 0 if all(checks.values()) else 1
    finally:
        observer.client.loop_stop()
        for name in ("hm-soak-llm", "hm-soak-perception"):
            docker("rm", "-f", name, check=False)
        llama.stop()


if __name__ == "__main__":
    raise SystemExit(main())
