"""Diagnosis: send "아 좀 덥네" through homemind-voice-text N times and record llm/request + llm/result per run."""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import IMAGE, REPO  # noqa: E402

import paho.mqtt.client as mqtt  # noqa: E402

ENV = {**os.environ, "MSYS_NO_PATHCONV": "1"}
lock = threading.Lock()
seen: list[tuple[float, str, dict]] = []
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"diag-{uuid.uuid4().hex[:6]}")
client.on_connect = lambda c, u, f, rc, p: [c.subscribe(t, 1) for t in ("pfn/v1/ai/llm/#",)]
client.on_message = lambda c, u, m: (lock.acquire(), seen.append((time.time(), m.topic, json.loads(m.payload))), lock.release())
client.connect("127.0.0.1", 1883, 30)
client.loop_start()
work = Path(tempfile.mkdtemp(prefix="hmdiag-"))
common = {"broker": {"host": "host.docker.internal", "port": 1883}, "contracts_dir": "/w/contracts", "simulated": True,
          "base_dir": "/w", "llm_base_url": "http://host.docker.internal:8080", "llm_model": "exaone-1.2b-homemind-v3",
          "capabilities_path": "services/exaone-edge/config/device-capabilities.json"}
(work / "c.json").write_text(json.dumps(common), "utf-8")
mounts = ["-v", f"{REPO.as_posix()}:/w", "-v", "hm-native-build-e2e:/build", "-v", f"{work.as_posix()}:/t"]
subprocess.run(["docker", "rm", "-f", "hm-diag-llm"], capture_output=True, env=ENV)
subprocess.run(["docker", "run", "-d", "--name", "hm-diag-llm", *mounts, IMAGE, "/build/services/homemind-llm/homemind-llm",
                "--config", "/t/c.json"], check=True, capture_output=True, env=ENV)
time.sleep(3)
rows = []
for i in range(int(sys.argv[1]) if len(sys.argv) > 1 else 20):
    start = time.time()
    run = subprocess.run(["docker", "run", "--rm", "-i", *mounts, IMAGE,
                          "/build/services/homemind-voice-text/homemind-voice-text", "--config", "/t/c.json"],
                         input="아 좀 덥네\n", capture_output=True, text=True, encoding="utf-8", env=ENV)
    deadline = time.time() + 30
    while time.time() < deadline:
        with lock:
            req = [d for at, t, d in seen if at >= start and t.endswith("/request")]
            res = [d for at, t, d in seen if at >= start and t.endswith("/result")]
        if req and res:
            break
        time.sleep(0.2)
    rows.append({"i": i, "rc": run.returncode, "requests": len(req), "results": [r["status"] for r in res],
                 "stdout": run.stdout.strip()[-120:], "stderr_tail": run.stderr.strip().splitlines()[-3:]})
    print(json.dumps(rows[-1], ensure_ascii=False), flush=True)
logs = subprocess.run(["docker", "logs", "hm-diag-llm"], capture_output=True, text=True, encoding="utf-8", env=ENV)
print("LLM LOG TAIL:", "\n".join((logs.stdout + logs.stderr).splitlines()[-15:]))
subprocess.run(["docker", "rm", "-f", "hm-diag-llm"], capture_output=True, env=ENV)
