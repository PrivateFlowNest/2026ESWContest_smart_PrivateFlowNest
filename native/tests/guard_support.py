"""Test support for agent A's native parity tests (hm_text, hm_guard, hm_parser, homemind-llm, voice-text).

PC x86_64 Docker only (simulation), never a Raspberry Pi result.

Isolated build: the tree is configured from a symlink farm that contains only hm_common and our folders, so a
half-written CMakeLists.txt of another library cannot break these tests (the shared build is hmnative.build()).

FakeLlama: fake llama-server (OpenAI-compatible /v1/chat/completions), no model. Each client gets a path
prefix (base_url http://host:port/<tag>) so the Python reference client and the C++ client (in Docker, via
host.docker.internal) see the same scripted replies and their request bodies can be compared byte for byte.
A script maps the request's utterance to one reply spec per attempt:

    {"content": "..."}               200, {"choices": [{"message": {"content": ...}}]}
    {"response": {...}}              200, json.dumps(obj)
    {"body": "...", "status": 200}   raw text body; "body_hex" for raw bytes
    {"status": 503}                  error status (standard reason phrase)
    {"close": True}                  close the connection without a response
    {"delay": 2.5, ...}              sleep first (timeouts)
"""

from __future__ import annotations

import json
import threading
import time
from collections import Counter, defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from hmnative import run_shell

NL = chr(10)
OWN_BUILD = "/build/own"
OWN_DIRS = ["libs/hm_common", "libs/hm_text", "libs/hm_guard", "libs/hm_parser", "services/homemind-llm",
            "services/homemind-voice-text"]
# Recreated in every container: /tmp/ws looks like the repository (contracts, services, native subset).
FARM = ("mkdir -p /tmp/ws/native/libs /tmp/ws/native/services && ln -sfn /w/contracts /tmp/ws/contracts && "
        "ln -sfn /w/services /tmp/ws/services && ln -sfn /w/native/CMakeLists.txt /tmp/ws/native/CMakeLists.txt && "
        "ln -sfn /w/native/cmake /tmp/ws/native/cmake && "
        + " && ".join(f"if [ -d /w/native/{d} ]; then ln -sfn /w/native/{d} /tmp/ws/native/{d}; fi" for d in OWN_DIRS))


def build_own(targets: str, *, build_type: str = "Debug") -> str:
    command = (f"{FARM} && cmake -S /tmp/ws/native -B {OWN_BUILD} -G Ninja -DCMAKE_BUILD_TYPE={build_type} "
               f"-DHM_CONTRACTS_DIR=/tmp/ws/contracts >/dev/null && cmake --build {OWN_BUILD} --target {targets} 2>&1 | tail -60")
    result = run_shell(command, timeout=1800)
    if result.returncode != 0 or "FAILED" in result.stdout or "error:" in result.stdout:
        raise RuntimeError("native build failed" + NL + result.stdout + NL + result.stderr)
    return result.stdout


def shell(command: str, **kwargs: Any):
    """run_shell with the farm in place (binaries under /build/own refer to /tmp/ws paths)."""
    return run_shell(f"{FARM} && {command}", **kwargs)


def probe(binary: str, requests: list[dict[str, Any]], *, args: str = "", timeout: float = 600) -> list[dict[str, Any]]:
    """JSONL probe with ASCII-escaped input (corpora contain lone surrogates a UTF-8 pipe cannot carry)."""
    lines = [json.dumps(item, ensure_ascii=True) for item in requests]
    result = shell(f"{OWN_BUILD}/{binary} {args}", stdin=NL.join(lines) + NL, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"{binary} exited {result.returncode}" + NL + result.stderr[-3000:])
    out = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    if len(out) != len(lines):
        raise RuntimeError(f"{binary}: {len(lines)} requests but {len(out)} answers" + NL + result.stderr[-3000:])
    return out


def completion(content: Any) -> dict[str, Any]:
    return {"choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}],
            "model": "fake-exaone", "object": "chat.completion"}


class FakeLlama:
    def __init__(self, script: dict[str, list[dict[str, Any]]] | None = None,
                 default: list[dict[str, Any]] | None = None) -> None:
        self.script = dict(script or {})
        self.scripts: dict[str, dict[str, list[dict[str, Any]]]] = {}  # per tag, overrides `script`
        self.default = default or [{"content": json.dumps({
            "intent": "NO_ACTION", "device": None, "action": None, "parameters": {}, "ambiguous": False,
            "safety_flags": [], "clarification_question": None}, ensure_ascii=False)}]
        self.log: dict[str, list[tuple[str | None, int, bytes]]] = defaultdict(list)
        self.counts: Counter[tuple[str, str | None]] = Counter()
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args: Any) -> None:  # quiet
                del args

            def _reply(self, status: int, body: bytes) -> None:
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self) -> None:  # noqa: N802
                self._reply(200, b'{"status": "ok"}')

            def do_POST(self) -> None:  # noqa: N802
                tag = self.path.strip("/").split("/")[0]
                body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                utterance = None
                try:
                    messages = json.loads(body.decode("utf-8"))["messages"]
                    utterance = json.loads(messages[-1]["content"]).get("utterance")
                except Exception:  # noqa: BLE001 - logged as None
                    pass
                with owner.lock:
                    attempt = owner.counts[(tag, utterance)]
                    owner.counts[(tag, utterance)] += 1
                    owner.log[tag].append((utterance, attempt, body))
                replies = owner.scripts.get(tag, owner.script).get(utterance or "", owner.default)
                spec = replies[min(attempt, len(replies) - 1)]
                if spec.get("delay"):
                    time.sleep(float(spec["delay"]))
                if spec.get("close"):
                    self.close_connection = True
                    return
                status = int(spec.get("status", 200))
                if "content" in spec:
                    payload = json.dumps(completion(spec["content"]), ensure_ascii=False).encode("utf-8")
                elif "response" in spec:
                    payload = json.dumps(spec["response"], ensure_ascii=False).encode("utf-8")
                elif "body_hex" in spec:
                    payload = bytes.fromhex(spec["body_hex"])
                else:
                    payload = str(spec.get("body", "{}")).encode("utf-8")
                self._reply(status, payload)

        self.server = ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return int(self.server.server_address[1])

    def url(self, tag: str, *, container: bool) -> str:
        host = "host.docker.internal" if container else "127.0.0.1"
        return f"http://{host}:{self.port}/{tag}"

    def bodies(self, tag: str) -> dict[str | None, list[bytes]]:
        out: dict[str | None, list[bytes]] = defaultdict(list)
        for utterance, _attempt, body in self.log[tag]:
            out[utterance].append(body)
        return out

    def __enter__(self) -> "FakeLlama":
        self.thread.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.server.shutdown()
        self.server.server_close()
