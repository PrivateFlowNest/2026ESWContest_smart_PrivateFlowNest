"""PC test helpers for native/: build in the Ubuntu 24.04 dev image, run JSONL probes, validate contracts.

Builds live in a named Docker volume per builder (hm-native-build-<builder>) so parallel builders never share
a build tree and incremental builds stay fast. Simulation only: this is the x86_64 PC build, not a Pi.

    from hmnative import build, run_probe, validate
    build()                                   # cmake configure + build everything (Debug)
    out = run_probe("libs/hm_common/hm_common_probe", [{"op": "resolve", ...}])
"""

from __future__ import annotations

import json
import os
import subprocess
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterable

REPO = Path(__file__).resolve().parents[2]
CONTRACTS = REPO / "contracts"
IMAGE = os.environ.get("HM_NATIVE_IMAGE", "homemind-native-dev:24.04")
BUILDER = os.environ.get("HM_NATIVE_BUILDER", "default")


def _docker(builder: str, command: str, *, stdin: str | None = None, timeout: float = 1800,
            extra_mounts: Iterable[str] = ()) -> subprocess.CompletedProcess[str]:
    args = ["docker", "run", "--rm", "-i", "-v", f"{REPO.as_posix()}:/w", "-v", f"hm-native-build-{builder}:/build",
            "-e", "HM_CONTRACTS_DIR=/w/contracts", "-e", "HM_BASE_DIR=/w"]
    for mount in extra_mounts:
        args += ["-v", mount]
    args += [IMAGE, "bash", "-c", command]
    env = {**os.environ, "MSYS_NO_PATHCONV": "1"}
    return subprocess.run(args, input=stdin, capture_output=True, text=True, encoding="utf-8", timeout=timeout,
                          env=env)


def build(builder: str = BUILDER, *, build_type: str = "Debug", cmake_args: str = "", target: str = "") -> str:
    command = (f"cmake -S /w/native -B /build -G Ninja -DCMAKE_BUILD_TYPE={build_type} {cmake_args} >/dev/null && "
               f"cmake --build /build {('--target ' + target) if target else ''} 2>&1 | tail -60")
    result = _docker(builder, command)
    if result.returncode != 0 or "FAILED" in result.stdout or "error:" in result.stdout:
        raise RuntimeError(f"native build failed\n{result.stdout}\n{result.stderr}")
    return result.stdout


def ctest(builder: str = BUILDER, regex: str = "") -> str:
    selection = f"-R '{regex}'" if regex else ""
    result = _docker(builder, f"cd /build && ctest --output-on-failure {selection} 2>&1 | tail -40")
    if "100% tests passed" not in result.stdout or result.returncode != 0:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def run_probe(binary: str, requests: Iterable[dict[str, Any]], *, args: str = "", builder: str = BUILDER,
              timeout: float = 600) -> list[dict[str, Any]]:
    """Feed JSON lines to /build/<binary> and parse one JSON line back per request."""

    lines = [json.dumps(item, ensure_ascii=False) for item in requests]
    result = _docker(builder, f"/build/{binary} {args}", stdin="\n".join(lines) + "\n", timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"{binary} exited {result.returncode}\n{result.stderr}")
    out = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    if len(out) != len(lines):
        raise RuntimeError(f"{binary}: {len(lines)} requests but {len(out)} answers\n{result.stderr}")
    return out


def run_shell(command: str, *, builder: str = BUILDER, timeout: float = 600, stdin: str | None = None,
              extra_mounts: Iterable[str] = ()) -> subprocess.CompletedProcess[str]:
    return _docker(builder, command, stdin=stdin, timeout=timeout, extra_mounts=extra_mounts)


@lru_cache(maxsize=None)
def _validator(schema_name: str):
    from jsonschema import Draft202012Validator, FormatChecker

    schema = json.loads((CONTRACTS / "pfn.v1" / schema_name).read_text(encoding="utf-8"))
    return Draft202012Validator(schema, format_checker=FormatChecker())


def validate(document: dict[str, Any], schema_name: str) -> None:
    errors = sorted(_validator(schema_name).iter_errors(document), key=str)
    if errors:
        raise AssertionError(f"{schema_name}: " + "; ".join(error.message for error in errors[:5]))


TOPIC_SCHEMAS = {
    "pfn/v1/ai/perception": "perception.schema.json",
    "pfn/v1/core/task/request": "task-request.schema.json",
    "pfn/v1/ai/llm/request": "llm-request.schema.json",
    "pfn/v1/ai/llm/result": "llm-result.schema.json",
}


def validate_topic_message(topic: str, document: dict[str, Any]) -> None:
    validate(document, TOPIC_SCHEMAS[topic])
    if topic == "pfn/v1/core/task/request" and document["intent"] == "device_control":
        validate(document["parameters"], "device-control.schema.json")
