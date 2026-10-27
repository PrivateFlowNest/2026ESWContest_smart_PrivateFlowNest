"""Comparable CPU benchmark for fixed-shape YOLOX ONNX candidates."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import platform
from statistics import mean, median
from time import perf_counter
from typing import Any

import numpy as np
import psutil


def _percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def benchmark_model(
    path: str | Path,
    *,
    runs: int = 100,
    warmup_runs: int = 10,
    threads: int = 4,
    streams: int = 2,
) -> dict[str, Any]:
    if runs < 1 or warmup_runs < 0 or threads < 1 or streams < 1:
        raise ValueError("runs/threads/streams must be positive and warmup non-negative")
    import onnxruntime as ort

    model_path = Path(path).resolve()
    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    options.intra_op_num_threads = threads
    options.inter_op_num_threads = 1
    process = psutil.Process()
    rss_before = process.memory_info().rss
    started = perf_counter()
    session = ort.InferenceSession(
        str(model_path),
        sess_options=options,
        providers=["CPUExecutionProvider"],
    )
    load_ms = (perf_counter() - started) * 1000.0
    input_meta = session.get_inputs()[0]
    shape = [1 if not isinstance(item, int) or item < 1 else item for item in input_meta.shape]
    if len(shape) != 4:
        raise ValueError(f"expected rank-4 image input, got {shape}")
    dtype = np.float32 if "float" in input_meta.type else np.uint8
    inputs = [np.zeros(shape, dtype=dtype) for _ in range(streams)]

    for index in range(warmup_runs):
        session.run(None, {input_meta.name: inputs[index % streams]})
    timings: list[float] = []
    for index in range(runs):
        started = perf_counter()
        session.run(None, {input_meta.name: inputs[index % streams]})
        timings.append((perf_counter() - started) * 1000.0)
    rss_after = process.memory_info().rss
    return {
        "model": model_path.stem,
        "path": str(model_path),
        "sha256": _sha256(model_path),
        "model_bytes": model_path.stat().st_size,
        "input_name": input_meta.name,
        "input_shape": shape,
        "providers": session.get_providers(),
        "threads": threads,
        "streams_simulated": streams,
        "warmup_runs": warmup_runs,
        "runs": runs,
        "load_ms": round(load_ms, 3),
        "latency_ms": {
            "mean": round(mean(timings), 3),
            "p50": round(median(timings), 3),
            "p95": round(_percentile(timings, 0.95), 3),
            "min": round(min(timings), 3),
            "max": round(max(timings), 3),
        },
        "throughput_fps": round(1000.0 / mean(timings), 3),
        "rss_before_bytes": rss_before,
        "rss_after_bytes": rss_after,
        "rss_growth_bytes": max(0, rss_after - rss_before),
    }


def benchmark_candidates(
    models: list[Path],
    *,
    runs: int,
    warmup_runs: int,
    threads: int,
    streams: int,
) -> dict[str, Any]:
    results = [
        benchmark_model(
            model,
            runs=runs,
            warmup_runs=warmup_runs,
            threads=threads,
            streams=streams,
        )
        for model in models
    ]
    fastest = min(results, key=lambda item: item["latency_ms"]["p95"])
    return {
        "schema_version": "1.0",
        "purpose": "PC deployment proxy only; final figure accuracy and Pi 5 performance are not measured",
        "system": {
            "platform": platform.platform(),
            "processor": platform.processor(),
            "logical_cpu_count": psutil.cpu_count(logical=True),
            "physical_cpu_count": psutil.cpu_count(logical=False),
            "memory_bytes": psutil.virtual_memory().total,
        },
        "results": results,
        "fastest_by_p95": fastest["model"],
        "unmeasured": [
            "final_figure_precision_recall",
            "small_figure_recall",
            "raspberry_pi_5_latency",
            "raspberry_pi_5_temperature",
            "full_stack_resource_contention",
        ],
    }


def write_report(report: dict[str, Any], path: str | Path) -> Path:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return target
