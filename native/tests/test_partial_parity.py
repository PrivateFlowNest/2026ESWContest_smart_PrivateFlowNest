"""hm_partial (C++) vs services/edge-ai/partial-intent (Python reference): text normalization, hashed features,
ONNX probabilities, predict() events, the two-update stream gate and the PARTIAL_INTENT_HINT perception.

PC x86_64 Docker only (simulation). Run with the partial-intent venv (numpy, onnxruntime, jsonschema):

    $env:HM_NATIVE_IMAGE = "homemind-native-dev:24.04"; $env:HM_NATIVE_BUILDER = "vision"
    services/edge-ai/partial-intent/.venv/Scripts/python -m pytest native/tests/test_partial_parity.py -s \
        -p no:cacheprovider --basetemp "$env:TEMP/hmpt/d1"
"""

from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import sys
import time
from pathlib import Path
from typing import Any

import pytest

os.environ.setdefault("HM_NATIVE_IMAGE", "homemind-native-dev:24.04")
os.environ.setdefault("HM_NATIVE_BUILDER", "vision")

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import CONTRACTS, REPO, build, run_shell, validate_topic_message  # noqa: E402

SERVICE = REPO / "services" / "edge-ai" / "partial-intent"
sys.path.insert(0, str(SERVICE / "src"))
np = pytest.importorskip("numpy")
pytest.importorskip("onnxruntime")
from homemind_partial_intent import features as py_features  # noqa: E402
from homemind_partial_intent import policy as py_policy  # noqa: E402
from homemind_partial_intent import taxonomy as py_taxonomy  # noqa: E402
from homemind_partial_intent.runtime import IntentRuntime, StreamIntentGate  # noqa: E402

PROBE = "libs/hm_partial/hm_partial_probe"
MODEL = SERVICE / "artifacts" / "partial-intent-v2.onnx"
MODEL_IN_CONTAINER = "/w/" + MODEL.relative_to(REPO).as_posix()
REGISTRY = json.loads((CONTRACTS / "pfn.v1" / "device-registry.json").read_text(encoding="utf-8"))
REPORT: dict[str, Any] = {"scope": f"PC Docker {os.environ['HM_NATIVE_IMAGE']} (simulation, not a Raspberry Pi)"}

EDGE_TEXTS = [
    "", " ", "a", "불", "불 꺼", "  Ｈｅｌｌｏ \t\n WORLD  ", "　방 안이  추워 ", "İstanbul TV",
    "ΟΔΟΣ ΣΑ σ Σ.", "ㅎㅏ ㄴㅡㄹ", "é café", "😀 에어컨 😀", "티비 TV tv Tv", "x\x00y 덥다",
    "가" * 600, " ".join(["환기"] * 120), "ẞ STRASSE", "Ⅻ ① ﬁ ㎏ ㍿", "\x1c\x1d덥\x1e\x1f네\x85",
    "에어컨은 켜지 마", "불 켜줘", "현관문 열어", "제품 이름이 뭐였지", "회의 중이라 조용히", "세탁기 남은 세탁 시간",
]
MALFORMED_BYTES = [b"abc\xff\xfe", "덥다".encode() + b"\xe2\x82", b"\xed\xa0\x80 tv", b"\xc0\xaf\xf5\x80", b"ok \xf0\x9f\x98"]


def _rows() -> list[dict[str, Any]]:
    rows = []
    for path in sorted((SERVICE / "data").rglob("*.jsonl")):
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                row = json.loads(line)
                rows.append({"text": row["text"], "intent": row.get("intent"),
                             "source": path.relative_to(SERVICE / "data").as_posix()})
    return rows


ROWS = _rows()
TEXTS = [row["text"] for row in ROWS] + EDGE_TEXTS


def probe(requests: list[dict[str, Any]], timeout: float = 1800, builder: str | None = None) -> list[dict[str, Any]]:
    lines = [json.dumps(item, ensure_ascii=False) for item in requests]
    extra = {"builder": builder} if builder else {}
    result = run_shell(f"/build/{PROBE}", stdin="\n".join(lines) + "\n", timeout=timeout, **extra)
    assert result.returncode == 0, result.stderr
    answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(answers) == len(lines), result.stderr
    for answer in answers:
        assert "error" not in answer, answer
    return answers


def _text_field(text: str | bytes) -> dict[str, str]:
    if isinstance(text, bytes):
        return {"text_b64": base64.b64encode(text).decode("ascii")}
    return {"text": text}


def _py_text(text: str | bytes) -> str:
    return text.decode("utf-8", "surrogateescape") if isinstance(text, bytes) else text


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build(target="hm_partial_probe hm_partial_tests")
    yield
    print("\n[partial-intent parity report]\n" + json.dumps(REPORT, ensure_ascii=False, indent=1))


@pytest.fixture(scope="module")
def runtime() -> IntentRuntime:
    return IntentRuntime(MODEL)


def test_unit_tests_pass() -> None:
    result = run_shell("/build/libs/hm_partial/hm_partial_tests")
    assert result.returncode == 0 and "PASSED" in result.stdout, result.stdout[-3000:] + result.stderr


def test_dataset_found() -> None:
    assert len(ROWS) >= 2600
    REPORT["texts"] = {"dataset_rows": len(ROWS), "edge_cases": len(EDGE_TEXTS), "malformed_bytes": len(MALFORMED_BYTES)}


def test_term_tables_match() -> None:
    [tables] = probe([{"op": "tables"}])
    assert tables["SEMANTIC_TERMS"] == [[key, list(value)] for key, value in py_features.SEMANTIC_TERMS.items()]
    assert tables["NON_ACTION_TERMS"] == list(py_features.NON_ACTION_TERMS)
    assert tables["REQUEST_TERMS"] == list(py_features.REQUEST_TERMS)
    assert tables["SUPPRESSION_PHRASES"] == list(py_policy.SUPPRESSION_PHRASES)
    assert tables["META_REFERENCE_PHRASES"] == list(py_policy.META_REFERENCE_PHRASES)
    assert tables["DIRECT_ACTION_TERMS"] == list(py_policy.DIRECT_ACTION_TERMS)
    assert tables["INTENTS"] == list(py_taxonomy.INTENTS)
    assert tables["HINTS"] == py_taxonomy.HINTS


def test_normalize_parity() -> None:
    inputs: list[str | bytes] = TEXTS + MALFORMED_BYTES
    answers = probe([{"op": "normalize", **_text_field(text)} for text in inputs])
    for text, answer in zip(inputs, answers):
        normalized = py_features.normalize_text(_py_text(text))
        encoded = normalized.encode("utf-8", errors="replace")
        assert answer["length"] == len(normalized), repr(text)
        assert answer["sha256"] == hashlib.sha256(encoded).hexdigest(), repr(text)
        assert answer["normalized"] == encoded.decode("utf-8"), repr(text)
        assert answer["malformed"] == any(0xD800 <= ord(c) <= 0xDFFF for c in normalized)
    REPORT["normalize"] = {"inputs": len(inputs), "identical": len(inputs)}


def test_feature_parity() -> None:
    answers = probe([{"op": "features", "text": text} for text in TEXTS])
    worst, exact, nonzero = 0.0, 0, 0
    for text, answer in zip(TEXTS, answers):
        vector = py_features.vectorize_one(text)
        indices = np.flatnonzero(vector)
        assert answer["indices"] == indices.tolist(), repr(text)
        native = np.asarray(answer["values"], dtype=np.float32)
        difference = float(np.abs(native - vector[indices]).max()) if indices.size else 0.0
        worst = max(worst, difference)
        exact += int(np.array_equal(native, vector[indices]))
        nonzero += int(indices.size)
    REPORT["features"] = {"texts": len(TEXTS), "nonzero_values": nonzero, "bit_exact_vectors": exact,
                          "max_abs_diff": worst}
    assert worst <= 1e-6


def test_probability_parity(runtime: IntentRuntime) -> None:
    answers = probe([{"op": "probabilities", "model": MODEL_IN_CONTAINER, "text": text} for text in TEXTS])
    worst, argmax_same = 0.0, 0
    for text, answer in zip(TEXTS, answers):
        assert answer["classes"] == list(runtime.classes)
        python = runtime.probabilities(text)
        values = np.asarray([python[label] for label in runtime.classes])
        native = np.asarray(answer["probabilities"])
        worst = max(worst, float(np.abs(native - values).max()))
        argmax_same += int(int(native.argmax()) == int(values.argmax()))
    REPORT["probabilities"] = {"texts": len(TEXTS), "max_abs_diff": worst, "same_argmax": argmax_same}
    assert worst <= 1e-5
    assert argmax_same == len(TEXTS)


def _comparable(event: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in event.items() if key not in {"event_id", "trace_id", "timestamp", "confidence"}}


def test_predict_parity(runtime: IntentRuntime) -> None:
    from homemind_partial_intent.validation import validate_event

    inputs: list[str | bytes] = TEXTS + MALFORMED_BYTES
    requests, expected = [], []
    for text in inputs:
        for is_final in (False, True):
            requests.append({"op": "predict", "model": MODEL_IN_CONTAINER, "is_final": is_final, **_text_field(text),
                             "trace_id": "trace-fixed"})
            expected.append(runtime.predict(_py_text(text), is_final=is_final, trace_id="trace-fixed"))
    answers = probe(requests)
    worst, results = 0.0, {}
    for request, python, native in zip(requests, expected, answers):
        validate_event(native)
        assert native["trace_id"] == "trace-fixed"
        assert _comparable(native) == _comparable(python), request
        worst = max(worst, abs(native["confidence"] - python["confidence"]))
        results[python["result"]] = results.get(python["result"], 0) + 1
    REPORT["predict"] = {"events": len(requests), "identical_fields": len(requests), "results": results,
                         "max_confidence_diff": worst}
    assert worst <= 1e-5


def _stream_updates(text: str) -> list[str]:
    words = text.split()
    cuts = sorted({max(1, int(np.ceil(len(words) * fraction))) for fraction in (0.6, 0.8, 1.0)})
    return [" ".join(words[:cut]) for cut in cuts]


def test_stream_gate_parity(runtime: IntentRuntime) -> None:
    """Per-row StreamIntentGate exactly as training._stream_gate_metrics, plus a final-update variant."""

    requests, expected = [], []
    for text in TEXTS:
        prefixes = _stream_updates(text) or [text]
        for final_last in (False, True):
            updates = [{"text": prefix, "is_final": final_last and index == len(prefixes) - 1}
                       for index, prefix in enumerate(prefixes)]
            gate = StreamIntentGate(runtime, confirmation_updates=2)
            expected.append([gate.update(u["text"], is_final=u["is_final"]) for u in updates])
            requests.append({"op": "stream", "model": MODEL_IN_CONTAINER, "confirmation_updates": 2, "updates": updates})
    answers = probe(requests)
    confirmed = updates_total = 0
    for request, python_events, answer in zip(requests, expected, answers):
        assert answer["threshold"] == 0.3
        assert len(answer["events"]) == len(python_events)
        for python, native in zip(python_events, answer["events"]):
            assert _comparable(native) == _comparable(python), request
            assert native["execution_allowed"] is False
            confirmed += int(native["result"] == "DETECTED")
            updates_total += 1
    REPORT["stream_gate"] = {"sequences": len(requests), "updates": updates_total, "confirmed_events": confirmed,
                             "identical": True}


def test_stream_gate_metrics_match_python(runtime: IntentRuntime) -> None:
    """Test-split stream metric (training._stream_gate_metrics method) with the native gate."""

    rows = [row for row in ROWS if row["source"] == "partial-intent-v2/test.jsonl"]
    requests = [{"op": "stream", "model": MODEL_IN_CONTAINER, "confirmation_updates": 2,
                 "updates": [{"text": prefix} for prefix in _stream_updates(row["text"])]} for row in rows]
    answers = probe(requests)

    def predicted(events: list[dict[str, Any]]) -> str:
        detected = [event["intent"] for event in events if event["result"] == "DETECTED"]
        return detected[-1] if detected else "NO_ACTION"

    native = [predicted(answer["events"]) for answer in answers]
    python = []
    for row in rows:
        gate = StreamIntentGate(runtime, confirmation_updates=2, threshold=0.3)
        python.append(predicted([gate.update(prefix) for prefix in _stream_updates(row["text"])]))
    assert native == python
    direct = [py_policy.requires_final_transcript(row["text"]) for row in rows]
    positive = [row["intent"] != "NO_ACTION" and not d for row, d in zip(rows, direct)]
    recall = sum(p and n == row["intent"] for p, n, row in zip(positive, native, rows)) / max(1, sum(positive))
    negatives = [row["intent"] == "NO_ACTION" for row in rows]
    false_positive = sum(neg and n != "NO_ACTION" for neg, n in zip(negatives, native)) / max(1, sum(negatives))
    REPORT["stream_metrics_test_split"] = {"rows": len(rows), "positive_recall": recall,
                                           "proactive_false_positive_rate": false_positive}


def test_hint_perception_is_contract_valid(runtime: IntentRuntime) -> None:
    devices = {device["device_id"]: device for device in REGISTRY["devices"]}
    events = [runtime.predict(text, is_final=True) for text in TEXTS[:400]]
    assert any(event["result"] == "DETECTED" for event in events)
    answers = probe([{"op": "hint", "event": event, "simulated": True} for event in events])
    published = 0
    for event, answer in zip(events, answers):
        perception = answer["perception"]
        if event["result"] != "DETECTED":
            assert perception is None
            continue
        published += 1
        validate_topic_message("pfn/v1/ai/perception", perception)
        assert perception["zone"] in REGISTRY["zones"]
        assert devices[perception["source_device_id"]]["controller_node_id"] == perception["source_node_id"]
        assert all(candidate in devices for candidate in perception["attributes"]["candidate_devices"])
        assert perception["attributes"]["execution_allowed"] is False
        assert perception["attributes"]["intent"] == event["intent"]
        assert math.isclose(perception["confidence"], round(event["confidence"], 4), abs_tol=1.00001e-4)
    REPORT["hint_perception"] = {"events": len(events), "published": published}


def test_utterance_stream_resets_per_utterance() -> None:
    updates = [{"text": "방 안이 점점 추워지는", "utterance_id": "u1"},
               {"text": "방 안이 점점 추워지는 것 같아", "utterance_id": "u1"},
               {"text": "방 안이 점점 추워지는", "utterance_id": "u2"},
               {"text": "방 안이 점점 추워지는 것 같아", "utterance_id": "u2"}]
    [answer] = probe([{"op": "utterances", "model": MODEL_IN_CONTAINER, "updates": updates}])
    results = [event["result"] for event in answer["events"]]
    assert results == ["INSUFFICIENT_EVIDENCE", "DETECTED", "INSUFFICIENT_EVIDENCE", "DETECTED"]


def test_latency_pc(runtime: IntentRuntime) -> None:
    release = os.environ["HM_NATIVE_BUILDER"] + "-release"
    build(builder=release, build_type="Release", target="hm_partial_probe")
    texts = [row["text"] for row in ROWS if row["source"] == "partial-intent-v2/test.jsonl"]
    [native] = probe([{"op": "bench", "model": MODEL_IN_CONTAINER, "texts": texts, "runs": 1000}], builder=release)
    runtime.predict(texts[0])
    timings = []
    for index in range(1000):
        started = time.perf_counter_ns()
        runtime.predict(texts[index % len(texts)])
        timings.append((time.perf_counter_ns() - started) / 1e6)
    REPORT["latency"] = {
        "label": "PC x86_64 (Ryzen 9 5900X); native = Docker Ubuntu 24.04 Release build, python = Windows venv; "
                 "end-to-end predict (normalize, features, ONNX, event). Not a Raspberry Pi measurement.",
        "native_p50_ms": round(native["p50_ms"], 4), "native_p95_ms": round(native["p95_ms"], 4),
        "python_p50_ms": round(float(np.percentile(timings, 50)), 4),
        "python_p95_ms": round(float(np.percentile(timings, 95)), 4),
    }
    assert native["p95_ms"] < 100
