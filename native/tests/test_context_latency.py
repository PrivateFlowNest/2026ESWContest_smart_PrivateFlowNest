"""(e) hm_context per-window latency (4 s window) on the PC: x86_64 Docker, NOT a Raspberry Pi measurement.

Stages: YAMNet ONNX Runtime + pooling, VAD features, activity head, feature event + fusion. Reported with
pytest -s; the only assertion is a generous real-time bound (one window per second).
"""

from __future__ import annotations

import numpy as np
import pytest

from context_support import (DATA, HEAD_JSON, POLICY, YAMNET_ONNX, container, ensure_built, probe, report,
                             require_models, write_f32)
from hmnative import run_shell


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    require_models()
    ensure_built()


def test_latency_per_window() -> None:
    stream = DATA / "stream.f32"
    if not stream.is_file():  # test_context_pipeline writes the real-audio stream; fall back to noise
        rng = np.random.default_rng(1)
        write_f32("stream.f32", (rng.standard_normal(120 * 16000) * 0.1).astype(np.float32))
    requests = [{"op": "bench", "model": container(YAMNET_ONNX), "head": container(HEAD_JSON),
                 "policy": container(POLICY), "input": container(stream), "threads": threads, "windows": 60,
                 "warmup": 3} for threads in (1, 2, 4)]
    answers = probe(requests)
    cpu = run_shell("grep -m1 'model name' /proc/cpuinfo; nproc").stdout.strip().replace("\n", " | ")
    summary = {"platform": "PC x86_64 Docker (not a Pi)", "cpu": cpu}
    for answer in answers:
        assert "error" not in answer, answer
        summary[f"threads={answer['threads']}"] = {
            stage: {k: round(v, 2) for k, v in answer[stage].items() if k in ("mean", "p50", "p95", "max")}
            for stage in ("yamnet_ms", "vad_ms", "head_ms", "fusion_ms", "total_ms")}
    report("e_latency", summary)
    assert answers[1]["total_ms"]["p95"] < 500.0, summary
