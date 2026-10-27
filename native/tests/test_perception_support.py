"""Shared helpers for the test_perception_*.py files (no tests here).

Normal mode builds through hmnative.build() (the whole native/ tree, binaries under /build).
HM_PERCEPTION_ISOLATED=1 configures a symlinked copy of native/ that only contains the libraries listed in
HM_PERCEPTION_LIBS (default: whatever configures), so work-in-progress folders of other builders cannot break
the configure step; binaries are then under /build/iso. Both run in the same per-builder Docker volume.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Any, Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import REPO, build, run_shell  # noqa: E402

ISOLATED = os.environ.get("HM_PERCEPTION_ISOLATED") == "1"
BIN = "/build/iso" if ISOLATED else "/build"
BASE_LIBS = ["hm_common", "hm_audio", "hm_stt"]


def optional_libs() -> list[str]:
    value = os.environ.get("HM_PERCEPTION_LIBS")
    if value is not None:
        return [item for item in value.split(",") if item]
    return [lib for lib in ("hm_text", "hm_guard", "hm_parser", "hm_context")
            if (REPO / "native" / "libs" / lib / "CMakeLists.txt").is_file()]


def build_targets(targets: str, *, build_type: str = "Debug", cmake_args: str = "") -> str:
    if not ISOLATED:
        return build(target=targets, build_type=build_type, cmake_args=cmake_args)
    libs = BASE_LIBS + optional_libs()
    links = " && ".join(
        ["rm -rf /build/iso-src && mkdir -p /build/iso-src/native/libs /build/iso-src/native/services",
         "for e in /w/*; do [ \"$(basename \"$e\")\" = native ] || ln -s \"$e\" /build/iso-src/; done",
         "ln -s /w/native/CMakeLists.txt /w/native/cmake /w/native/tests /build/iso-src/native/"]
        + [f"ln -s /w/native/libs/{lib} /build/iso-src/native/libs/{lib}" for lib in libs]
        + ["ln -s /w/native/services/homemind-perception /build/iso-src/native/services/homemind-perception"])
    command = (f"{links} && cmake -S /build/iso-src/native -B /build/iso -G Ninja -DCMAKE_BUILD_TYPE={build_type} "
               f"-DHM_CONTRACTS_DIR=/w/contracts {cmake_args} > /tmp/cfg.log 2>&1 || {{ tail -40 /tmp/cfg.log; exit 1; }}; "
               f"cmake --build /build/iso --target {targets} 2>&1 | tail -60")
    result = run_shell(command, timeout=1800)
    if result.returncode != 0 or "FAILED" in result.stdout or "error:" in result.stdout:
        raise RuntimeError(f"isolated native build failed ({libs})\n{result.stdout}\n{result.stderr}")
    return result.stdout


def run_jsonl(binary: str, requests: Iterable[dict[str, Any]], *, args: str = "", mounts: Iterable[str] = (),
              timeout: float = 1200) -> list[dict[str, Any]]:
    lines = [json.dumps(item, ensure_ascii=False) for item in requests]
    result = run_shell(f"{BIN}/{binary} {args}", stdin="\n".join(lines) + "\n", extra_mounts=list(mounts),
                       timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"{binary} exited {result.returncode}\n{result.stderr}")
    answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    if len(answers) != len(lines):
        raise RuntimeError(f"{binary}: {len(lines)} requests, {len(answers)} answers\n{result.stderr}")
    return answers
