"""Partial-intent hints in homemind-perception (agent F, P4 "부분 인텐트"): Vosk partial transcripts while an
utterance is captured -> partial-intent gate per utterance -> PARTIAL_INTENT_HINT (execution_allowed false).

PC x86_64 Docker (simulation, not a Raspberry Pi) unless marked QEMU arm64. Real speech: AIHub 71405 (home, near,
HomeMind command manifest) copied from HM_AIHUB_DIR into the pytest folder; nothing is committed.

1. Vosk partial strings, 100 ms chunks, on the same clips:
   a. C++ hm_vosk (libvosk 0.3.45 linux x86_64)  vs  the Python VoskBackend algorithm on the same .so (ctypes, in the
      container; the transliteration is checked against the real VoskBackend on Windows first)      -> must be equal
   b. C++  vs  the Windows Python path as it is (vosk 0.3.45 wheel, its own libvosk.dll)            -> reported
2. Hints: the service (real Vosk + real partial-intent ONNX, whole-file and VAD modes) vs the Python partial-intent
   runtime (StreamIntentGate per utterance) fed with the same partials                          -> must be equal
3. PC latency per 100 ms chunk (Release), model load, memory; Python Windows RTF for comparison.
4. QEMU arm64: hm_vosk tests and partials with the aarch64 libvosk (emulation; timing is not a Pi 5 figure).

    $env:HM_NATIVE_BUILDER = "perception2"; $env:PYTHONUTF8 = "1"
    services/runtime/.venv/Scripts/python -m pytest native/tests/test_perception_partial.py -s -p no:cacheprovider \
        --basetemp "$env:TEMP/hmpt/f2"
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import wave
from pathlib import Path
from typing import Any

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import CONTRACTS, REPO, build, run_shell, validate_topic_message  # noqa: E402
from test_perception_support import BIN, build_targets  # noqa: E402

from homemind_runtime.audio.stt import write_pcm16_wav  # noqa: E402

STT = REPO / "services" / "edge-ai" / "speech-to-text"
STT_PYTHON = STT / ".venv" / "Scripts" / "python.exe"
VOSK_MODEL = STT / "models" / "vosk-model-small-ko-0.22"
VOSK_MODEL_C = "/w/" + VOSK_MODEL.relative_to(REPO).as_posix()
MANIFEST = STT / "artifacts" / "manifests" / "aihub-71405-home-near-homemind-100.jsonl"
INTENT_MODEL = REPO / "services" / "edge-ai" / "partial-intent" / "artifacts" / "partial-intent-v2.onnx"
HARNESS = f"{BIN}/services/homemind-perception/homemind-perception-harness"
VOSK_X64 = "/build/_deps/vosk-linux-x86_64-0.3.45/libvosk.so"
PROBE = "libs/hm_vosk/hm_vosk_probe"
CLIPS = int(os.environ.get("HM_PARTIAL_CLIPS", "60"))
REPORT: dict[str, Any] = {"scope": "PC x86_64 Docker unless noted (simulation, not a Raspberry Pi)"}

# services/edge-ai/speech-to-text VoskBackend.transcribe() line by line, on a libvosk loaded with ctypes
# (the vosk wheel is a cffi wrapper over the same C calls). Run as: python vosk_ref.py LIB MODEL CHUNK_MS < wav paths
VOSK_REFERENCE = r'''
import ctypes, json, os, sys, wave
lib_path, model_dir, chunk_ms = sys.argv[1], sys.argv[2], int(sys.argv[3])
if os.name == "nt":
    os.add_dll_directory(os.path.dirname(lib_path))
lib = ctypes.CDLL(lib_path)
lib.vosk_model_new.restype = ctypes.c_void_p
lib.vosk_model_new.argtypes = [ctypes.c_char_p]
lib.vosk_recognizer_new.restype = ctypes.c_void_p
lib.vosk_recognizer_new.argtypes = [ctypes.c_void_p, ctypes.c_float]
lib.vosk_recognizer_accept_waveform.restype = ctypes.c_int
lib.vosk_recognizer_accept_waveform.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
for name in ("vosk_recognizer_result", "vosk_recognizer_partial_result", "vosk_recognizer_final_result"):
    getattr(lib, name).restype = ctypes.c_char_p
    getattr(lib, name).argtypes = [ctypes.c_void_p]
lib.vosk_recognizer_free.argtypes = [ctypes.c_void_p]
lib.vosk_set_log_level(-1)
model = lib.vosk_model_new(model_dir.encode())
for path in sys.stdin.read().split():
    with wave.open(path, "rb") as handle:
        assert handle.getnchannels() == 1 and handle.getsampwidth() == 2 and handle.getframerate() == 16000
        frames = handle.readframes(handle.getnframes())
    recognizer = lib.vosk_recognizer_new(model, 16000)
    step = max(1, 16000 * chunk_ms // 1000) * 2
    finalized, partials, last_partial = [], [], ""
    for offset in range(0, len(frames), step):
        data = frames[offset:offset + step]
        if lib.vosk_recognizer_accept_waveform(recognizer, data, len(data)):
            text = json.loads(lib.vosk_recognizer_result(recognizer).decode("utf-8")).get("text", "").strip()
            if text:
                finalized.append(text)
        else:
            text = json.loads(lib.vosk_recognizer_partial_result(recognizer).decode("utf-8")).get("partial", "").strip()
            if text and text != last_partial:
                partials.append(text)
                last_partial = text
    text = json.loads(lib.vosk_recognizer_final_result(recognizer).decode("utf-8")).get("text", "").strip()
    if text:
        finalized.append(text)
    lib.vosk_recognizer_free(recognizer)
    print(json.dumps({"partials": partials, "finalized": finalized}, ensure_ascii=False), flush=True)
'''

# The real reference (Windows, vosk wheel): VoskBackend(chunk_ms=100).transcribe() + its timing.
WINDOWS_BACKEND = r'''
import json, sys, time
sys.path.insert(0, sys.argv[1])
from homemind_stt.backends import VoskBackend
backend = VoskBackend(sys.argv[2], chunk_ms=int(sys.argv[3]))
for path in sys.stdin.read().split():
    started = time.perf_counter()
    result = backend.transcribe(path)
    print(json.dumps({"partials": list(result.partials), "text": result.text,
                      "ms": (time.perf_counter() - started) * 1000}, ensure_ascii=False), flush=True)
'''


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_targets("homemind-perception-harness hm_vosk_probe hm_vosk_tests")


@pytest.fixture(scope="module")
def work(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("partial")


@pytest.fixture(scope="module", autouse=True)
def report():
    yield
    print("\nPERCEPTION_PARTIAL_REPORT " + json.dumps(REPORT, ensure_ascii=False))


def shell(work: Path, command: str, timeout: float = 1800, builder: str | None = None):
    kwargs = {"builder": builder} if builder else {}
    return run_shell(command, extra_mounts=[f"{work.as_posix()}:/t"], timeout=timeout, **kwargs)


@pytest.fixture(scope="module")
def clips(work) -> list[Path]:
    if not VOSK_MODEL.is_dir() or not MANIFEST.is_file():
        pytest.skip("Vosk model or AIHub manifest missing")
    folder = work / "clips"
    folder.mkdir(exist_ok=True)
    out = []
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        source = Path(json.loads(line)["audio_path"].replace("\\", "/"))
        if not source.is_file():
            continue
        with wave.open(str(source), "rb") as handle:
            ok = (handle.getnchannels(), handle.getsampwidth(), handle.getframerate()) == (1, 2, 16000)
            long_enough = handle.getnframes() >= 16000  # hm_audio pads shorter files to 1 s, Python does not
        if ok and long_enough:
            target = folder / f"c{len(out):03d}.wav"
            shutil.copyfile(source, target)
            out.append(target)
        if len(out) >= CLIPS:
            break
    if len(out) < 5:
        pytest.skip("AIHub clips not available")
    return out


def native_partials(work: Path, clips: list[Path], *, builder: str | None = None, library: str = VOSK_X64,
                    raw: bool = False) -> tuple[dict, list[dict]]:
    requests = [{"op": "info"}] + [{"op": "partials", "wav": f"/t/clips/{c.name}", "chunk_ms": 100, "raw": raw}
                                   for c in clips]
    result = run_shell(f"/build/{PROBE} --library {library} --model {VOSK_MODEL_C}",
                       stdin="\n".join(json.dumps(r) for r in requests) + "\n", extra_mounts=[f"{work.as_posix()}:/t"],
                       timeout=3600, **({"builder": builder} if builder else {}))
    assert result.returncode == 0, result.stderr[-3000:]
    answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(answers) == len(requests), result.stderr[-2000:]
    return answers[0], answers[1:]


@pytest.fixture(scope="module")
def native(work, clips) -> list[dict]:
    info, answers = native_partials(work, clips)
    REPORT["vosk_native_debug_build"] = {"clips": len(clips), "model_load_ms": round(info["model_load_ms"]),
                                         "rss_mb_after_model": round(info["rss_kb_after_model"] / 1024)}
    return answers


def stats(values: list[float]) -> dict[str, float]:
    array = np.sort(np.asarray(values, dtype=np.float64))
    pick = lambda q: float(array[min(len(array) - 1, int(q * len(array)))])  # noqa: E731
    return {"n": int(len(array)), "p50": round(pick(0.5), 3), "p95": round(pick(0.95), 3),
            "p99": round(pick(0.99), 3), "max": round(float(array[-1]), 3), "mean": round(float(array.mean()), 3)}


# ---- 1. partial strings --------------------------------------------------------------------------------------
def test_unit_tests_pass(work) -> None:
    result = shell(work, "/build/libs/hm_vosk/hm_vosk_tests --gtest_brief=1 2>&1 | tail -3")
    assert "PASSED" in result.stdout and "FAILED" not in result.stdout, result.stdout
    REPORT["gtest_hm_vosk"] = result.stdout.strip().splitlines()[-1]


def test_reference_transliteration_equals_voskbackend_on_windows(work, clips) -> None:
    """The ctypes transliteration of VoskBackend gives the same partials as VoskBackend itself (same libvosk.dll)."""

    if not STT_PYTHON.is_file():
        pytest.skip("speech-to-text venv missing")
    dll = STT / ".venv" / "Lib" / "site-packages" / "vosk" / "libvosk.dll"
    (work / "vosk_ref.py").write_text(VOSK_REFERENCE, encoding="utf-8")
    (work / "vosk_backend.py").write_text(WINDOWS_BACKEND, encoding="utf-8")
    subset = "\n".join(str(c) for c in clips[:15])
    env = {**os.environ, "PYTHONUTF8": "1"}
    ref = subprocess.run([str(STT_PYTHON), "-I", "-X", "utf8", str(work / "vosk_ref.py"), str(dll), str(VOSK_MODEL), "100"],
                         input=subset, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800, env=env)
    real = subprocess.run([str(STT_PYTHON), "-I", "-X", "utf8", str(work / "vosk_backend.py"), str(STT / "src"), str(VOSK_MODEL), "100"],
                          input=subset, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800, env=env)
    assert ref.returncode == 0 and real.returncode == 0, ref.stderr[-2000:] + real.stderr[-2000:]
    ours = [json.loads(line)["partials"] for line in ref.stdout.splitlines()]
    theirs = [json.loads(line)["partials"] for line in real.stdout.splitlines()]
    assert len(ours) == len(theirs) == 15
    assert ours == theirs
    REPORT["transliteration_vs_voskbackend_windows"] = {"clips": 15, "identical": True}


def test_partials_match_python_algorithm_on_the_same_libvosk(work, clips, native) -> None:
    (work / "vosk_ref.py").write_text(VOSK_REFERENCE, encoding="utf-8")
    paths = "\n".join(f"/t/clips/{c.name}" for c in clips)
    result = run_shell(f"python3 -I /t/vosk_ref.py {VOSK_X64} {VOSK_MODEL_C} 100", stdin=paths + "\n",
                       extra_mounts=[f"{work.as_posix()}:/t"], timeout=3600)
    assert result.returncode == 0, result.stderr[-3000:]
    python = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(python) == len(clips)
    same = [p["partials"] == n["partials"] for p, n in zip(python, native)]
    same_finalized = [p["finalized"] == n["finalized"] + ([n["final"]] if n["final"] else []) for p, n in
                      zip(python, native)]
    REPORT["partials_vs_python_same_libvosk_x86_64"] = {
        "clips": len(clips), "identical_partial_sequences": sum(same),
        "partials": sum(len(n["partials"]) for n in native), "identical_final_text": sum(same_finalized),
        "clips_with_endpoint_segments": sum(bool(n["finalized"]) for n in native)}
    assert all(same), [i for i, ok in enumerate(same) if not ok]
    # Vosk's own final hypothesis is not used by the service; it is reported only (see the history test below).


def test_vosk_output_depends_on_decode_history(work, clips) -> None:
    """libvosk is not stateless: decoding the same clips again in the same process (shared model) can give other
    partials. C++ and the Python algorithm agree pass by pass, so this is libvosk's behaviour, not the wrapper's."""

    (work / "vosk_ref.py").write_text(VOSK_REFERENCE, encoding="utf-8")
    subset = clips[:20]
    paths = [f"/t/clips/{c.name}" for c in subset] * 2
    requests = "\n".join(json.dumps({"op": "partials", "wav": path}) for path in paths)
    native = run_shell(f"/build/{PROBE} --library {VOSK_X64} --model {VOSK_MODEL_C}", stdin=requests + "\n",
                       extra_mounts=[f"{work.as_posix()}:/t"], timeout=3600)
    python = run_shell(f"python3 -I /t/vosk_ref.py {VOSK_X64} {VOSK_MODEL_C} 100", stdin="\n".join(paths) + "\n",
                       extra_mounts=[f"{work.as_posix()}:/t"], timeout=3600)
    assert native.returncode == 0 and python.returncode == 0, native.stderr[-2000:] + python.stderr[-2000:]
    cc = [json.loads(line)["partials"] for line in native.stdout.splitlines() if line.strip()]
    py = [json.loads(line)["partials"] for line in python.stdout.splitlines() if line.strip()]
    first, second = cc[:len(subset)], cc[len(subset):]
    REPORT["vosk_history_dependence"] = {
        "clips": len(subset), "same_partials_second_pass": sum(a == b for a, b in zip(first, second)),
        "cpp_equals_python_both_passes": cc == py}
    assert cc == py


def test_partials_vs_windows_python_wheel(work, clips, native) -> None:
    """The Windows Python path as it is (vosk wheel, its own libvosk.dll): reported, not required to be equal."""

    if not STT_PYTHON.is_file():
        pytest.skip("speech-to-text venv missing")
    (work / "vosk_backend.py").write_text(WINDOWS_BACKEND, encoding="utf-8")
    result = subprocess.run([str(STT_PYTHON), "-I", "-X", "utf8", str(work / "vosk_backend.py"), str(STT / "src"), str(VOSK_MODEL),
                             "100"], input="\n".join(str(c) for c in clips), capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=3600, env={**os.environ, "PYTHONUTF8": "1"})
    assert result.returncode == 0, result.stderr[-3000:]
    windows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(windows) == len(clips)
    same = [w["partials"] == n["partials"] for w, n in zip(windows, native)]
    strings_native = sum(len(n["partials"]) for n in native)
    strings_common = sum(len(set(w["partials"]) & set(n["partials"])) for w, n in zip(windows, native))
    seconds = [wave.open(str(c), "rb").getnframes() / 16000 for c in clips]
    REPORT["partials_vs_windows_python_wheel"] = {
        "clips": len(clips), "identical_partial_sequences": sum(same), "differing_clips": [i for i, s in enumerate(same) if not s],
        "native_partial_strings": strings_native, "strings_also_in_windows_sequence": strings_common,
        "identical_final_text": sum(w["text"] == " ".join(n["finalized"] + ([n["final"]] if n["final"] else [])).strip()
                                    for w, n in zip(windows, native)),
        "windows_python_rtf": round(sum(w["ms"] for w in windows) / 1000 / sum(seconds), 3)}
    assert sum(same) >= len(clips) // 2  # builds differ in float details; most sequences still agree


# ---- 2. hints ---------------------------------------------------------------------------------------------
def python_hints(partials: list[str], trace: str) -> list[dict]:
    """Python partial-intent runtime: one StreamIntentGate per utterance, fed with the partials (never final)."""

    from homemind_partial_intent.runtime import IntentRuntime, StreamIntentGate

    runtime = python_hints.runtime = getattr(python_hints, "runtime", None) or IntentRuntime(INTENT_MODEL)
    gate = StreamIntentGate(runtime, confirmation_updates=2)
    devices = json.loads((CONTRACTS / "pfn.v1" / "ai-action-map.json").read_text(encoding="utf-8"))["devices"]
    out = []
    for text in partials:
        event = gate.update(text, is_final=False, trace_id=trace)
        assert event["execution_allowed"] is False
        if event["result"] != "DETECTED":
            continue
        candidates = sorted({devices[d]["device_id"] for d in event["candidate_devices"]
                             if d in devices and devices[d].get("enabled") and isinstance(devices[d].get("device_id"), str)})
        out.append({"intent": event["intent"], "confidence": round(event["confidence"], 4),
                    "candidate_devices": candidates, "model_version": event["model"]["version"], "trace_id": trace})
    return out


def hint_view(document: dict) -> dict:
    attributes = document["attributes"]
    return {"intent": attributes["intent"], "confidence": document["confidence"],
            "candidate_devices": attributes["candidate_devices"], "model_version": attributes["model_version"],
            "trace_id": document["trace_id"]}


def check_hint_document(document: dict, partial_texts: list[str]) -> None:
    validate_topic_message("pfn/v1/ai/perception", document)
    attributes = document["attributes"]
    assert document["observation_type"] == "PARTIAL_INTENT_HINT" and document["source_device_id"] == "MIC-INDOOR-01"
    assert attributes["execution_allowed"] is False and attributes["requires_confirmation"] is True
    dumped = json.dumps(document, ensure_ascii=False)
    for text in partial_texts:
        assert text not in dumped  # no transcript in the hint


def parse_run(stdout: str, stderr: str) -> tuple[list[tuple[str, dict]], dict[str, list[str]], dict]:
    messages = []
    for line in stdout.splitlines():
        if line.startswith("pfn/v1/"):
            topic, payload = line.split(" ", 1)
            messages.append((topic, json.loads(payload)))
    partials: dict[str, list[str]] = {}
    marker = "partial (debug_transcripts) trace="
    for line in stderr.splitlines():
        if marker in line:
            rest = line.split(marker, 1)[1]
            trace, text = rest.split(" ", 1)
            partials.setdefault(trace, []).append(text[1:-1])
    summary = json.loads([line for line in stderr.splitlines() if line.startswith('{"')][-1])
    return messages, partials, summary


def write_config(work: Path, name: str, **extra) -> str:
    document = {"simulated": True, "partial_intent": {"enabled": True, "vosk_library": VOSK_X64}, **extra}
    (work / name).write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
    return f"/t/{name}"


def test_service_whole_mode_hints_match_python(work, clips, native) -> None:
    if not INTENT_MODEL.is_file():
        pytest.skip("partial-intent model missing")
    expected = {i: python_hints(n["partials"], "t") for i, n in enumerate(native)}
    with_hints = [i for i, e in expected.items() if e]
    chosen = with_hints + [i for i in range(len(clips)) if i not in with_hints][:max(5, 20 - len(with_hints))]
    cfg = write_config(work, "whole.json", wav_voice="whole")
    # Each clip in a fresh process on both sides (libvosk output depends on what the model decoded before).
    loop = " ; ".join(
        f"echo ==={i}; echo '{{\"op\": \"partials\", \"wav\": \"/t/clips/{clips[i].name}\"}}' | "
        f"/build/{PROBE} --library {VOSK_X64} --model {VOSK_MODEL_C}; echo ---SERVICE; "
        f"{HARNESS} --config {cfg} --wav /t/clips/{clips[i].name} --fast --stdout --stt-fake-text '불 켜줘' "
        f"--context-scripted --debug-transcripts 2>/tmp/err{i}.txt; echo ---ERR; cat /tmp/err{i}.txt" for i in chosen)
    result = shell(work, loop, timeout=3600)
    assert result.returncode == 0, result.stderr[-3000:]
    compared = hints_total = 0
    for block in result.stdout.split("===")[1:]:
        index = int(block.split("\n", 1)[0])
        probe_out, rest = block.split("\n", 1)[1].split("---SERVICE", 1)
        fresh = json.loads(probe_out.strip().splitlines()[-1])
        out, err = rest.split("---ERR", 1)
        messages, partials, summary = parse_run(out, err)
        [(trace, texts)] = partials.items() if partials else [("", [])]
        assert texts == fresh["partials"], index  # the service streams exactly the clip, decoded like the probe
        hints = [d for t, d in messages if t == "pfn/v1/ai/perception" and d["observation_type"] == "PARTIAL_INTENT_HINT"]
        for document in hints:
            check_hint_document(document, texts)
        task = [d for t, d in messages if t == "pfn/v1/core/task/request"]
        assert len(task) == 1
        if trace:
            assert task[0]["trace_id"] == trace  # the final decision carries the utterance trace of its hints
        assert [hint_view(d) for d in hints] == python_hints(texts, trace or "t"), index
        assert summary["partial_intent"]["hints"] == len(hints)
        compared += 1
        hints_total += len(hints)
    REPORT["service_whole_mode_hints"] = {"clips": compared, "clips_with_hints": len(with_hints),
                                          "hints": hints_total, "identical_to_python_gate": True}
    assert compared == len(chosen)


def test_service_vad_mode_hints_match_python(work, clips) -> None:
    """Many utterances in one stream (energy VAD): one gate per utterance, hints == Python fed with the partials."""

    if not INTENT_MODEL.is_file():
        pytest.skip("partial-intent model missing")
    rng = np.random.default_rng(7)
    parts = [(0.0005 * rng.standard_normal(16000)).astype(np.float32)]
    for clip in clips[:30]:
        with wave.open(str(clip), "rb") as handle:
            parts.append(np.frombuffer(handle.readframes(handle.getnframes()), dtype="<i2").astype(np.float32) / 32768.0)
        parts.append((0.0005 * rng.standard_normal(int(1.8 * 16000))).astype(np.float32))
    write_pcm16_wav(work / "stream.wav", np.concatenate(parts))
    cfg = write_config(work, "vad.json", wav_voice="vad")
    result = shell(work, f"{HARNESS} --config {cfg} --wav /t/stream.wav --fast --stdout --stt-fake-text '아 좀 덥네' "
                         f"--context-scripted --debug-transcripts", timeout=3600)
    assert result.returncode == 0, result.stderr[-3000:]
    messages, partials, summary = parse_run(result.stdout, result.stderr)
    hints = [d for t, d in messages if t == "pfn/v1/ai/perception" and d["observation_type"] == "PARTIAL_INTENT_HINT"]
    decisions = {d["trace_id"] for t, d in messages if t in ("pfn/v1/core/task/request", "pfn/v1/ai/llm/request")}
    by_trace: dict[str, list[dict]] = {}
    for document in hints:
        check_hint_document(document, [t for texts in partials.values() for t in texts])
        by_trace.setdefault(document["trace_id"], []).append(hint_view(document))
    expected = {trace: python_hints(texts, trace) for trace, texts in partials.items()}
    assert {t: h for t, h in expected.items() if h} == by_trace
    assert set(by_trace) <= decisions  # every hint's utterance reached a final decision with the same trace
    REPORT["service_vad_mode_hints"] = {
        "utterances": summary["partial_intent"]["utterances"], "voice_decisions": len(summary["voice"]),
        "partials": summary["partial_intent"]["partials"], "hints": len(hints),
        "identical_to_python_gate": True, "chunk_ms_debug_build": summary["partial_intent"]["chunk_ms"]}
    assert summary["partial_intent"]["utterances"] >= 25


# ---- 3. PC latency ------------------------------------------------------------------------------------------
def test_latency_per_chunk_pc(work, clips) -> None:
    release = os.environ.get("HM_NATIVE_BUILDER", "default") + "-release"
    build(builder=release, build_type="Release", target="hm_vosk_probe homemind-perception-harness")
    subset = clips[:30]
    info, answers = native_partials(work, subset, builder=release, raw=True)
    chunks = [value for answer in answers for value in answer["chunk_ms_raw"]]
    seconds = sum(answer["audio_seconds"] for answer in answers)
    REPORT["latency_pc_x86_64_release"] = {
        "clips": len(subset), "audio_seconds": round(seconds, 1), "chunk_ms_per_100ms_chunk": stats(chunks),
        "rtf": round(sum(a["total_ms"] for a in answers) / 1000 / seconds, 3),
        "recognizer_create_ms": stats([a["recognizer_ms"] for a in answers]),
        "first_chunk_ms": stats([a["chunk_ms_raw"][0] for a in answers]),
        "model_load_ms": round(info["model_load_ms"]), "rss_mb_after_model": round(info["rss_kb_after_model"] / 1024),
        "rss_mb_peak_seen": round(max(a["rss_kb"] for a in answers) / 1024)}
    # The service's own measurement (worker accept() per 100 ms ring block, Release, VAD stream of 30 clips).
    result = shell(work, f"/build/services/homemind-perception/homemind-perception-harness --config /t/vad.json "
                         f"--wav /t/stream.wav --fast --stdout --stt-fake-text '아 좀 덥네' --context-scripted",
                   builder=release, timeout=3600) if (work / "stream.wav").is_file() else None
    if result is not None and result.returncode == 0:
        summary = json.loads([line for line in result.stderr.splitlines() if line.startswith('{"')][-1])
        REPORT["latency_pc_x86_64_release"]["service_worker_chunk_ms"] = summary["partial_intent"]["chunk_ms"]
        REPORT["latency_pc_x86_64_release"]["service_gate_ms"] = summary["partial_intent"]["gate_ms"]


# ---- 4. QEMU arm64 -----------------------------------------------------------------------------------------
def arm64_step(step):
    """Runs an arm64 Docker step. Another agent's WSL start can wipe the Docker VM's binfmt registration at any time
    ("exec format error"): re-register QEMU right before every attempt and retry, as the lead instructed."""

    last: Exception | None = None
    for _ in range(4):
        subprocess.run(["docker", "run", "--privileged", "--rm", "tonistiigi/binfmt", "--install", "arm64"],
                       capture_output=True, timeout=300)
        try:
            result = step()
        except Exception as error:  # noqa: BLE001 - only the binfmt failure is retried
            if "exec format error" not in str(error):
                raise
            last = error
            continue
        if isinstance(result, subprocess.CompletedProcess) and "exec format error" in result.stdout + result.stderr:
            last = RuntimeError(result.stderr[-500:])
            continue
        return result
    raise AssertionError(f"arm64 step kept failing with exec format error (binfmt): {last}")


def test_arm64_libvosk_under_qemu(work, clips, native) -> None:
    if os.environ.get("HM_PARTIAL_ARM64", "1") != "1":
        pytest.skip("HM_PARTIAL_ARM64=0")
    image = "homemind-native-dev:24.04-arm64"
    if subprocess.run(["docker", "image", "inspect", image], capture_output=True).returncode != 0:
        pytest.skip("arm64 dev image missing")
    import hmnative

    builder = os.environ.get("HM_NATIVE_BUILDER", "default") + "-arm64"
    previous = hmnative.IMAGE
    hmnative.IMAGE = image
    try:
        arm64_step(lambda: build(builder=builder, build_type="Release", target="hm_vosk_probe hm_vosk_tests"))
        library = "/build/_deps/vosk-linux-aarch64-0.3.45/libvosk.so"
        checks = arm64_step(lambda: shell(
            work, f"uname -m; ldd {library}; objdump -T {library} | grep -o 'GLIBC[A-Z_]*_[0-9.]*' | sort -uV | tail -3; "
                  "objdump -T " + library + " | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1; "
                  "/build/libs/hm_vosk/hm_vosk_tests --gtest_brief=1 2>&1 | tail -2", builder=builder))
        subset = clips[:5]
        started = time.time()
        info, answers = arm64_step(lambda: native_partials(work, subset, builder=builder, library=library, raw=True))
        elapsed = time.time() - started
    finally:
        hmnative.IMAGE = previous
    # x86_64 reference with the same decode history (a fresh process, the same 5 clips in order)
    _, x86 = native_partials(work, subset)
    same = [a["partials"] == n["partials"] for a, n in zip(answers, x86)]
    REPORT["qemu_arm64"] = {
        "checks": checks.stdout.strip().splitlines(), "clips": len(subset), "identical_partials_to_x86_64": sum(same),
        "partials_arm64": sum(len(a["partials"]) for a in answers),
        "model_load_ms_emulated": round(info["model_load_ms"]), "rss_mb_after_model": round(info["rss_kb_after_model"] / 1024),
        "rtf_emulated_not_a_pi": round(sum(a["total_ms"] for a in answers) / 1000 / sum(a["audio_seconds"] for a in answers), 2),
        "wall_seconds": round(elapsed, 1)}
    assert "aarch64" in checks.stdout and "PASSED" in checks.stdout
    assert all(a["chunks"] > 0 for a in answers)
