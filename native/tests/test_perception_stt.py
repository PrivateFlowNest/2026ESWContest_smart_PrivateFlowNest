"""hm_stt with a real whisper.cpp (x86_64 build of b4938, PC x86_64 Docker; not Raspberry Pi numbers).

One whisper-server container (lora-v4 q5_0, 8 threads, CPU) serves both clients:
  - C++ hm_stt_probe in a second container (http://host.docker.internal:<port>)
  - Python WhisperTranscriber + homemind_stt WhisperCppHttpBackend on Windows (http://127.0.0.1:<port>)
Both send the same PCM16 WAV bytes, so the transcripts must be identical. Also: whisper-cli fallback, latency, and
homemind-perception with real whisper on real Korean command clips (AIHub 71405 validation, local only).

Needs native/.deps/x64-tools/bin/whisper-{server,cli} (see README) and the model; skipped otherwise.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import IMAGE, REPO, run_shell, validate_topic_message  # noqa: E402
from test_perception_support import BIN, build_targets, run_jsonl  # noqa: E402

from homemind_audio.audio import load_wav_16k_mono  # noqa: E402
from homemind_runtime.audio.stt import WhisperTranscriber, write_pcm16_wav  # noqa: E402

PORT = int(os.environ.get("HM_PERCEPTION_WHISPER_PORT", "18178"))
CONTAINER = "hm-perception-whisper-test"
TOOLS = REPO / "native" / ".deps" / "x64-tools" / "bin"
MODEL_REL = "services/edge-ai/speech-to-text/artifacts/training/whisper-small-homemind-lora-v4/ggml-whisper-small-homemind-lora-v4-q5_0.bin"
MANIFEST = REPO / "services/edge-ai/speech-to-text/artifacts/manifests/aihub-71405-home-near-homemind-100.jsonl"
CLIPS = int(os.environ.get("HM_PERCEPTION_STT_CLIPS", "20"))
REPORT: dict[str, object] = {}
TRANSCRIPTS: list[str] = []

pytestmark = pytest.mark.skipif(not ((TOOLS / "whisper-server").is_file() and (REPO / MODEL_REL).is_file()),
                                reason="x86_64 whisper tools or model missing")


def _docker(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(["docker", *args], capture_output=True, text=True, encoding="utf-8", check=check,
                          env={**os.environ, "MSYS_NO_PATHCONV": "1"})


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_targets("hm_stt_probe homemind-perception-harness")


@pytest.fixture(scope="module")
def server():
    _docker("rm", "-f", CONTAINER, check=False)
    _docker("run", "-d", "--rm", "--name", CONTAINER, "-p", f"127.0.0.1:{PORT}:8178", "-v", f"{REPO.as_posix()}:/w:ro",
            IMAGE, "/w/native/.deps/x64-tools/bin/whisper-server", "-m", f"/w/{MODEL_REL}", "-l", "ko", "-t", "8",
            "-ng", "--host", "0.0.0.0", "--port", "8178")
    started = time.perf_counter()
    deadline = time.time() + 120
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{PORT}/", timeout=2).read()
            break
        except Exception:  # noqa: BLE001
            time.sleep(0.5)
    else:
        logs = _docker("logs", CONTAINER, check=False)
        _docker("rm", "-f", CONTAINER, check=False)
        pytest.fail("whisper-server did not start\n" + logs.stdout[-2000:] + logs.stderr[-2000:])
    REPORT["server_start_seconds"] = round(time.perf_counter() - started, 1)
    yield {"windows": f"http://127.0.0.1:{PORT}", "docker": f"http://host.docker.internal:{PORT}"}
    _docker("rm", "-f", CONTAINER, check=False)


@pytest.fixture(scope="module")
def work(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("stt")


@pytest.fixture(scope="module", autouse=True)
def report():
    yield
    print("\nPERCEPTION_STT_REPORT " + json.dumps(REPORT, ensure_ascii=False))


@pytest.fixture(scope="module")
def clips(work) -> list[dict]:
    if not MANIFEST.is_file():
        pytest.skip("AIHub manifest missing")
    out = []
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        item = json.loads(line)
        source = Path(item["audio_path"].replace("\\", "/"))
        if not source.is_file():
            continue
        target = work / f"clip{len(out):02d}.wav"
        write_pcm16_wav(target, load_wav_16k_mono(source))  # local copy, deleted with the temp folder
        out.append({"path": target, "reference": item["reference"], "seconds": float(item.get("duration_seconds", 0))})
        if len(out) >= CLIPS:
            break
    if len(out) < 5:
        pytest.skip("AIHub clips not available on this PC")
    return out


def _normalise(text: str) -> str:
    return "".join(ch for ch in text if ch.isalnum())


def _cer(reference: str, hypothesis: str) -> float:
    ref, hyp = _normalise(reference), _normalise(hypothesis)
    previous = list(range(len(hyp) + 1))
    for i, r in enumerate(ref, 1):
        current = [i]
        for j, h in enumerate(hyp, 1):
            current.append(min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (r != h)))
        previous = current
    return previous[-1] / max(1, len(ref))


def _percentile(values, q):
    return round(float(np.percentile(np.asarray(values), q)), 1)


def test_server_transcripts_match_python_client(work, server, clips) -> None:
    requests = [{"wav": f"/t/{c['path'].name}", "server_url": server["docker"], "model_version": "lora-v4"} for c in clips]
    native = run_jsonl("libs/hm_stt/hm_stt_probe", requests, mounts=[f"{work.as_posix()}:/t"])
    errors = [item["error"] for item in native if "error" in item]
    assert not errors, errors
    from homemind_stt.backends import WhisperCppHttpBackend

    transcriber = WhisperTranscriber({}, backend=WhisperCppHttpBackend(server["windows"], model_version="lora-v4"))
    python, py_latency = [], []
    for clip in clips:
        result = transcriber.transcribe(load_wav_16k_mono(clip["path"]))
        python.append(result.text)
        py_latency.append(result.latency_ms)
    TRANSCRIPTS.extend(item["text"] for item in native)
    identical = sum(n["text"] == p for n, p in zip(native, python))
    assert identical == len(clips), [i for i, (n, p) in enumerate(zip(native, python)) if n["text"] != p]
    latency = [item["latency_ms"] for item in native]
    audio = [item["seconds"] for item in native]
    REPORT["server"] = {
        "clips": len(clips), "native_equals_python_transcript": identical,
        "cer_mean": round(float(np.mean([_cer(c["reference"], n["text"]) for c, n in zip(clips, native)])), 4),
        "exact_after_normalise": sum(_normalise(c["reference"]) == _normalise(n["text"]) for c, n in zip(clips, native)),
        "native_latency_ms_p50": _percentile(latency, 50), "native_latency_ms_p95": _percentile(latency, 95),
        "python_latency_ms_p50": _percentile(py_latency, 50),
        "audio_seconds_mean": round(float(np.mean(audio)), 2),
        "real_time_factor_p50": round(float(np.median(np.asarray(latency) / 1000.0 / np.asarray(audio))), 3),
        "label": "PC x86_64 Docker (CPU, 8 threads), not Raspberry Pi",
    }


def test_cli_fallback_and_latency(work, server, clips) -> None:
    subset = clips[:5]
    cli = "/w/native/.deps/x64-tools/bin/whisper-cli"
    model = f"/w/{MODEL_REL}"
    requests = [{"wav": f"/t/{c['path'].name}", "cli": cli, "model": model, "threads": 8, "model_version": "lora-v4"}
                for c in subset]
    # dead server URL + CLI available -> every utterance falls back to whisper-cli
    requests += [{"wav": f"/t/{c['path'].name}", "server_url": "http://127.0.0.1:9", "cli": cli, "model": model,
                  "threads": 8} for c in subset[:2]]
    # dead server, no CLI -> error (the perception service logs it and keeps running)
    requests.append({"wav": f"/t/{subset[0]['path'].name}", "server_url": "http://127.0.0.1:9"})
    lines = "\n".join(json.dumps(item) for item in requests) + "\n"
    probe = run_shell(f"{BIN}/libs/hm_stt/hm_stt_probe; echo LEFT=$(ls -d /tmp/hm-stt-* 2>/dev/null | wc -l)",
                      stdin=lines, extra_mounts=[f"{work.as_posix()}:/t"], timeout=1200)
    assert probe.returncode == 0, probe.stderr
    native = [json.loads(line) for line in probe.stdout.splitlines() if line.startswith("{")]
    leftovers = [line for line in probe.stdout.splitlines() if line.startswith("LEFT=")][0][5:]
    assert leftovers == "0"  # every temp WAV folder removed
    cli_results = native[: len(subset)]
    assert all(item.get("backend") == "cli" for item in native[: len(subset) + 2]), native
    assert "error" in native[-1]
    server_side = run_jsonl("libs/hm_stt/hm_stt_probe",
                            [{"wav": f"/t/{c['path'].name}", "server_url": server["docker"]} for c in subset],
                            mounts=[f"{work.as_posix()}:/t"])
    same = sum(a["text"] == b["text"] for a, b in zip(cli_results, server_side))
    REPORT["cli"] = {"clips": len(subset), "same_text_as_server": same,
                     "cli_latency_ms_p50": _percentile([i["latency_ms"] for i in cli_results], 50),
                     "server_latency_ms_p50": _percentile([i["latency_ms"] for i in server_side], 50),
                     "note": "cli loads the model for every utterance", "temp_dirs_left": leftovers}


def test_perception_with_real_whisper_on_real_speech(work, server, clips) -> None:
    gap = (0.0007 * np.random.default_rng(3).standard_normal(int(1.2 * 16_000))).astype(np.float32)
    parts = []
    for clip in clips[:6]:
        parts += [gap, load_wav_16k_mono(clip["path"])]
    parts.append(gap)
    scene = work / "scene.wav"
    write_pcm16_wav(scene, np.concatenate(parts))
    config = {"simulated": True, "whisper_server_url": server["docker"], "wav_voice": "vad",
              "whisper_cli_path": "/nonexistent"}
    (work / "cfg.json").write_text(json.dumps(config), encoding="utf-8")
    result = run_shell(f"{BIN}/services/homemind-perception/homemind-perception-harness --config /t/cfg.json "
                       f"--wav /t/scene.wav --fast --stdout --context-scripted",
                       extra_mounts=[f"{work.as_posix()}:/t"], timeout=900)
    assert result.returncode == 0, result.stderr[-3000:]
    messages = []
    for line in result.stdout.splitlines():
        if line.startswith("pfn/v1/"):
            topic, payload = line.split(" ", 1)
            document = json.loads(payload)
            validate_topic_message(topic, document)
            messages.append((topic, document))
    summary = json.loads([line for line in result.stderr.splitlines() if line.startswith('{"')][-1])
    voice = summary["voice"]
    assert len(voice) >= 5, voice
    assert all(item["decision"] != "empty" for item in voice)
    echo = "\n".join(line for line in result.stdout.splitlines() if line.startswith("[voice]"))
    for text in TRANSCRIPTS[:6]:  # transcripts never reach logs or console lines without debug_transcripts
        if len(text) >= 4:
            assert text not in result.stderr and text not in echo
    REPORT["perception_real_whisper"] = {
        "utterances": len(voice), "decisions": [item["decision"] for item in voice],
        "stt_ms": [item["stt_ms"] for item in voice],
        "topics": sorted({topic for topic, _ in messages}),
        "parser": "keyword double unless libs/hm_parser is linked",
    }


# ---- full native binary vs `python -m homemind_runtime.perception` with real whisper ----------------------
IGNORED = {"event_id", "trace_id", "task_id", "request_id", "utterance_id", "observed_at_ms", "requested_at_ms",
           "expires_at_ms", "valid_until_ms"}


def _strip(document):
    if isinstance(document, dict):
        return {k: _strip(v) for k, v in document.items() if k not in IGNORED}
    if isinstance(document, list):
        return [_strip(v) for v in document]
    return document


def _topic_lines(text: str) -> list[tuple[str, dict]]:
    out = []
    for line in text.splitlines():
        if line.startswith("pfn/v1/"):
            topic, payload = line.split(" ", 1)
            out.append((topic, json.loads(payload)))
    return out


def _summary(stderr: str) -> dict:
    return json.loads([line for line in stderr.splitlines() if line.startswith("{") and '"windows"' in line][-1])


def _native_linked() -> bool:
    result = run_shell(f"nm -C {BIN}/services/homemind-perception/homemind-perception | "
                       "grep -cE 'hm::VoiceParser::handle|hm::AudioContextPipeline::process' || true")
    return result.stdout.strip().splitlines()[-1] == "2"


@pytest.mark.parametrize("voice", ["vad", "whole"])
def test_full_native_vs_python_cli_with_real_whisper(work, server, clips, voice) -> None:
    build_targets("homemind-perception")
    if not _native_linked():
        pytest.skip("libs/hm_context and libs/hm_parser not linked (doubles only)")
    gap = (0.0007 * np.random.default_rng(9).standard_normal(int(1.2 * 16_000))).astype(np.float32)
    chosen = clips[:6] if voice == "vad" else clips[6:7]
    parts = []
    for clip in chosen:
        parts += [gap, load_wav_16k_mono(clip["path"])]
    parts += [gap, gap, gap]
    scene = work / f"full-{voice}.wav"
    write_pcm16_wav(scene, np.concatenate(parts))
    common = {"simulated": True, "wav_voice": voice, "whisper_cli_path": "/nonexistent"}
    (work / f"py-{voice}.json").write_text(json.dumps({**common, "whisper_server_url": server["windows"]}), "utf-8")
    (work / f"cc-{voice}.json").write_text(json.dumps({**common, "whisper_server_url": server["docker"]}), "utf-8")
    python = REPO / "services" / "runtime" / ".venv" / "Scripts" / "python.exe"
    py = subprocess.run([str(python), "-m", "homemind_runtime.perception", "--config", str(work / f"py-{voice}.json"),
                         "--wav", str(scene), "--fast", "--stdout"], capture_output=True, text=True, encoding="utf-8",
                        timeout=1800, cwd=REPO / "services" / "runtime", env={**os.environ, "PYTHONUTF8": "1"})
    assert py.returncode == 0, py.stderr[-3000:]
    cc = run_shell(f"{BIN}/services/homemind-perception/homemind-perception --config /t/cc-{voice}.json "
                   f"--wav /t/{scene.name} --fast --stdout", extra_mounts=[f"{work.as_posix()}:/t"], timeout=1800)
    assert cc.returncode == 0, cc.stderr[-3000:]
    py_messages, cc_messages = _topic_lines(py.stdout), _topic_lines(cc.stdout)
    for topic, document in cc_messages:
        validate_topic_message(topic, document)
    py_summary, cc_summary = _summary(py.stderr), _summary(cc.stderr)
    assert cc_summary["windows"] == py_summary["windows"]
    py_perc = [_strip(d) for t, d in py_messages if t == "pfn/v1/ai/perception"]
    cc_perc = [_strip(d) for t, d in cc_messages if t == "pfn/v1/ai/perception"]
    assert cc_perc == py_perc
    py_voice = [(t, _strip(d)) for t, d in py_messages if t != "pfn/v1/ai/perception"]
    cc_voice = [(t, _strip(d)) for t, d in cc_messages if t != "pfn/v1/ai/perception"]
    assert [t for t, _ in cc_voice] == [t for t, _ in py_voice]
    for (topic, a), (_, b) in zip(cc_voice, py_voice):
        if voice == "vad":  # Python --fast races the context path for VAD utterances; whole mode is exact
            a.pop("context", None)
            b.pop("context", None)
        assert a == b, (topic, a, b)
    keys = ("decision", "device", "action", "topics", "chars") + (("context",) if voice == "whole" else ())
    assert [{k: v.get(k) for k in keys} for v in cc_summary["voice"]] == \
           [{k: v.get(k) for k in keys} for v in py_summary["voice"]]
    REPORT[f"full_native_vs_python_cli/{voice}"] = {
        "windows": cc_summary["windows"], "perceptions": len(cc_perc), "voice_messages": len(cc_voice),
        "decisions": [v["decision"] for v in cc_summary["voice"]],
        "native_stt_ms": [v["stt_ms"] for v in cc_summary["voice"]],
        "python_stt_ms": [v["stt_ms"] for v in py_summary["voice"]],
        "label": "PC x86_64 Docker (native) / Windows (Python), simulated, same whisper-server",
    }
