"""hm_context AudioContextPipeline vs services/runtime audio/context.py on minutes of real audio.

(d) One stream (~4 min) stitched from AIHub test-split clips of every activity label plus digital silence,
    4 s windows with 1 s hop, the same timestamps on both sides. Raw labels, fused context types,
    quiet_required and the transition sequence must be identical; numeric fields are compared after the
    Python rounding (6 decimals) and every other field must be equal. PC x86_64 Docker, not a device test.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

import numpy as np
import pytest

from context_support import (ACTIVITY_JOBLIB, DATA, HEAD_JSON, POLICY, YAMNET_ONNX, YAMNET_SAVED_MODEL, container,
                             ensure_built, load_clip, probe, python_head, real_clips, report, require_models,
                             tf_extractor, write_f32)

BASE_MS = 1_792_600_000_000  # 2026-10-21T16:26:40Z
IDS = ("event_id", "trace_id")


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    require_models()
    ensure_built()


@pytest.fixture(scope="module")
def stream() -> np.ndarray:
    clips = real_clips()
    by_label: dict[str, list[np.ndarray]] = {}
    for label, path, start, end in clips:
        by_label.setdefault(label, []).append(load_clip(path, start, end))
    rng = np.random.default_rng(3)
    blocks = by_label["IN_PERSON_MEETING"] + [np.zeros(8 * 16000, np.float32),
                                              (rng.standard_normal(4 * 16000) * 1e-3).astype(np.float32)]
    for label in ("TV_MEDIA", "SELF_TALK", "UNKNOWN", "ZOOM_SPEAKER"):
        blocks += by_label[label]
    waveform = np.concatenate(blocks).astype(np.float32)
    write_f32("stream.f32", waveform)
    return waveform


def python_run(waveform: np.ndarray, *, presence_zone: str, simulated: bool) -> list:
    from homemind_runtime.audio.context import AudioContextPipeline

    pipeline = AudioContextPipeline(activity_model=ACTIVITY_JOBLIB, yamnet_model=YAMNET_SAVED_MODEL, policy_path=POLICY,
                                    presence_zone=presence_zone, simulated=simulated, extractor=tf_extractor(),
                                    head=python_head())
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    updates = []
    for start in range(0, waveform.size - 64000 + 1, 16000):
        end = start + 64000
        updates.append(pipeline.process(waveform[start:end], timestamp=epoch + timedelta(milliseconds=BASE_MS + end // 16),
                                        elapsed_seconds=end / 16000))
    return updates


def native_run(*, presence_zone: str, simulated: bool) -> dict:
    new, run = probe([
        {"op": "pipeline_new", "yamnet_model": container(YAMNET_ONNX), "activity_head": container(HEAD_JSON),
         "fusion_policy": container(POLICY), "presence_zone": presence_zone, "simulated": simulated, "threads": 2},
        {"op": "pipeline_run", "input": container(DATA / "stream.f32"), "window": 64000, "hop": 16000,
         "base_timestamp_ms": BASE_MS},
    ])
    assert "error" not in new and "error" not in run, (new, run)
    return run


def numeric_diff(a, b, path="") -> tuple[float, list[str]]:
    """Max abs difference over numbers and the list of non-numeric field paths that differ."""
    if isinstance(a, dict) and isinstance(b, dict):
        worst, fields = 0.0, ([f"{path}.<keys>"] if set(a) != set(b) else [])
        for key in set(a) & set(b):
            if key in IDS:
                continue
            d, f = numeric_diff(a[key], b[key], f"{path}.{key}")
            worst, fields = max(worst, d), fields + f
        return worst, fields
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        return abs(float(a) - float(b)), []
    return 0.0, ([] if a == b else [path])


@pytest.mark.parametrize("presence_zone,simulated", [("UNKNOWN", False), ("DESK", True)])
def test_pipeline_parity_real_audio(stream, presence_zone: str, simulated: bool) -> None:
    import time

    started = time.perf_counter()
    python = python_run(stream, presence_zone=presence_zone, simulated=simulated)
    python_seconds = time.perf_counter() - started
    native = native_run(presence_zone=presence_zone, simulated=simulated)["updates"]
    assert len(python) == len(native) > 180

    def sequence(updates, key):
        return [u[key] if isinstance(u, dict) else getattr(u, key) for u in updates]

    raw_py, raw_cc = sequence(python, "raw_label"), sequence(native, "raw_label")
    ctx_py = [u.context_event for u in python]
    ctx_cc = [u["context_event"] for u in native]
    transitions_py = [(i, e["transition"], e["context_type"], e["quiet_required"]) for i, e in enumerate(ctx_py)
                      if e["transition"] != "UNCHANGED"]
    transitions_cc = [(i, e["transition"], e["context_type"], e["quiet_required"]) for i, e in enumerate(ctx_cc)
                      if e["transition"] != "UNCHANGED"]
    feature_worst, context_worst, feature_fields, context_fields = 0.0, 0.0, set(), set()
    exact_feature = exact_context = 0
    context_samples = []
    for index, (py, cc) in enumerate(zip(python, native)):
        d, fields = numeric_diff(py.feature_event, cc["feature_event"])
        feature_worst, feature_fields = max(feature_worst, d), feature_fields | set(fields)
        exact_feature += int(d == 0 and not fields)
        d, fields = numeric_diff(py.context_event, cc["context_event"])
        context_worst, context_fields = max(context_worst, d), context_fields | set(fields)
        exact_context += int(d == 0 and not fields)
        if (d or fields) and len(context_samples) < 4:
            only_py = [e for e in py.context_event["evidence"] if e not in cc["context_event"]["evidence"]]
            only_cc = [e for e in cc["context_event"]["evidence"] if e not in py.context_event["evidence"]]
            context_samples.append({"window": index, "python": only_py, "native": only_cc,
                                    "confidence": [py.context_event["confidence"], cc["context_event"]["confidence"]],
                                    "raw_confidence": [py.raw_confidence, cc["raw_confidence"]]})
    # Python's own feature events (+ the same configured presence events) through the C++ fusion: isolates the
    # fusion port from the YAMNet ONNX-vs-TensorFlow numeric noise.
    replay = []
    for update in python:
        event = update.feature_event
        if presence_zone != "UNKNOWN":
            replay.append({**{k: event[k] for k in ("schema_version", "trace_id", "timestamp", "valid_until")},
                           "event_id": f"presence-{len(replay)}", "source": "pc.scenario",
                           "source_node_id": "PC-SIMULATION", "event_type": "PRESENCE", "simulated": True,
                           "payload": {"presence_zone": presence_zone, "confidence": 0.9}})
        replay.append(event)
    answers = probe([{"op": "fusion_new", "policy": container(POLICY)}] +
                    [{"op": "fusion_ingest", "event": event} for event in replay])
    fused = [a["context_event"] for a in answers[1:] if a.get("reason") == "fused"]
    strip = lambda e: {k: v for k, v in e.items() if k != "event_id"}  # noqa: E731
    replay_equal = sum(strip(a) == strip(b) for a, b in zip(fused, ctx_py))
    speech_equal = sum(py.speech_ratio == cc["speech_ratio"] for py, cc in zip(python, native))
    raw_conf = max(abs(py.raw_confidence - cc["raw_confidence"]) for py, cc in zip(python, native))
    latency = [cc["latency_ms"] for cc in native]
    summary = {
        "presence_zone": presence_zone, "windows": len(python), "audio_seconds": round(stream.size / 16000, 1),
        "raw_labels_identical": raw_py == raw_cc,
        "raw_label_counts": {label: raw_py.count(label) for label in sorted(set(raw_py))},
        "transitions_identical": transitions_py == transitions_cc, "transitions": len(transitions_py),
        "context_types_identical": [e["context_type"] for e in ctx_py] == [e["context_type"] for e in ctx_cc],
        "quiet_required_identical": [e["quiet_required"] for e in ctx_py] == [e["quiet_required"] for e in ctx_cc],
        "speech_ratio_identical": f"{speech_equal}/{len(python)}",
        "raw_confidence_max_abs_diff": raw_conf,
        "feature_events_exactly_equal": f"{exact_feature}/{len(python)}",
        "feature_event_max_numeric_diff": feature_worst, "feature_event_other_fields_differing": sorted(feature_fields),
        "context_events_exactly_equal": f"{exact_context}/{len(python)}",
        "context_event_max_numeric_diff": context_worst, "context_event_other_fields_differing": sorted(context_fields),
        "context_event_difference_samples": context_samples,
        "replay_python_feature_events_through_cpp_fusion_equal": f"{replay_equal}/{len(ctx_py)}",
        "native_latency_ms_mean": float(np.mean(latency)), "native_latency_ms_p95": float(np.percentile(latency, 95)),
        "python_seconds_per_window": python_seconds / len(python),
        "transition_sequence": transitions_py,
    }
    report(f"d_pipeline_{presence_zone.lower()}", summary)
    assert raw_py == raw_cc, summary
    assert transitions_py == transitions_cc, summary
    assert summary["context_types_identical"] and summary["quiet_required_identical"], summary
    assert speech_equal == len(python), summary
    assert len(fused) == len(ctx_py) and replay_equal == len(ctx_py), summary
    assert feature_worst <= 2e-5 and not feature_fields, summary
