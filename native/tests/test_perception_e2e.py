"""homemind-perception vs services/runtime perception.py: WAV file mode end to end (PC x86_64 Docker, simulated).

1. Service-logic parity (no dependency on the C++ models): the Python service runs a WAV with its own context
   pipeline (real YAMNet + activity head + fusion when the artifacts and TensorFlow are there, else the scripted
   fake pipeline of services/runtime tests with the real fusion) and records every window's fused event. The C++
   harness replays those events (--context-replay) on the same WAV; the published perception sequences must match
   (topics, observation types, zones, confidences, attributes, transitions; ids/timestamps excluded) and the
   C++ windows must have the same stream times.
2. Full native pipeline (libs/hm_context + libs/hm_parser): `python -m homemind_runtime.perception --wav ... --fast
   --stdout` against `homemind-perception --wav ... --fast --stdout` (skipped until both libraries are linked).
3. Voice decisions with a fake transcript on both sides (real parser when linked, keyword double otherwise).

Every emitted message is validated with hmnative.validate_topic_message.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import REPO, run_shell, validate_topic_message  # noqa: E402
from test_perception_support import BIN, build_targets  # noqa: E402

from homemind_runtime.action_map import ActionMap  # noqa: E402
from homemind_runtime.audio.stt import SttResult, WhisperTranscriber, write_pcm16_wav  # noqa: E402
from homemind_runtime.config import RuntimeConfig  # noqa: E402
from homemind_runtime.perception import DEFAULTS, CapturePublisher, PerceptionService, resolve_settings, run_with  # noqa: E402

HARNESS = f"{BIN}/services/homemind-perception/homemind-perception-harness"
BINARY = f"{BIN}/services/homemind-perception/homemind-perception"
SR = 16_000
AIHUB = Path(os.environ.get("HM_AIHUB_DIR", "E:/HomeMind-Datasets/raw/aihub"))
PYTHON = REPO / "services" / "runtime" / ".venv" / "Scripts" / "python.exe"
REPORT: dict[str, object] = {}
IGNORED = {"event_id", "trace_id", "task_id", "request_id", "utterance_id", "observed_at_ms", "requested_at_ms",
           "expires_at_ms", "valid_until_ms"}


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_targets("homemind-perception homemind-perception-harness homemind_perception_tests")


@pytest.fixture(scope="module")
def work(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("e2e")


@pytest.fixture(scope="module", autouse=True)
def report(work):
    yield
    print("\nPERCEPTION_E2E_REPORT " + json.dumps(REPORT, ensure_ascii=False))


# ---- helpers ----------------------------------------------------------------------------------------
def tone(seconds, amplitude=0.3, freq=220.0):
    t = np.arange(int(seconds * SR)) / SR
    return (amplitude * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def noise(seconds, amplitude=0.0005, seed=0):
    return (amplitude * np.random.default_rng(seed).standard_normal(int(seconds * SR))).astype(np.float32)


def strip_volatile(document):
    if isinstance(document, dict):
        return {key: strip_volatile(value) for key, value in document.items() if key not in IGNORED}
    if isinstance(document, list):
        return [strip_volatile(item) for item in document]
    return document


def parse_stdout(text: str) -> list[tuple[str, dict]]:
    out = []
    for line in text.splitlines():
        if line.startswith("pfn/v1/"):
            topic, payload = line.split(" ", 1)
            out.append((topic, json.loads(payload)))
    return out


def summary_from(stderr: str) -> dict:
    for line in reversed(stderr.splitlines()):
        if line.startswith("{") and '"windows"' in line:
            return json.loads(line)
    raise AssertionError("no summary line on stderr:\n" + stderr[-3000:])


def replay_report(stderr: str) -> dict:
    for line in stderr.splitlines():
        if line.startswith("REPLAY "):
            return json.loads(line[len("REPLAY "):])
    raise AssertionError("no REPLAY line:\n" + stderr[-3000:])


def write_config(work: Path, name: str, extra: dict) -> str:
    config = {"broker": {"host": "127.0.0.1", "port": 1883}, "simulated": True, **extra}
    (work / name).write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    return f"/t/{name}"


def run_native(work: Path, args: str, *, binary: str = HARNESS, timeout: float = 900) -> subprocess.CompletedProcess:
    result = run_shell(f"{binary} {args}", extra_mounts=[f"{work.as_posix()}:/t"], timeout=timeout)
    assert result.returncode == 0, result.stderr[-4000:]
    return result


def validate_all(messages: list[tuple[str, dict]]) -> None:
    for topic, document in messages:
        validate_topic_message(topic, document)


def compare_perceptions(python_docs: list[dict], native_docs: list[dict]) -> dict:
    assert len(native_docs) == len(python_docs), (len(native_docs), len(python_docs))
    for index, (py, cc) in enumerate(zip(python_docs, native_docs)):
        assert strip_volatile(cc) == strip_volatile(py), (index, strip_volatile(cc), strip_volatile(py))
        for doc in (py, cc):
            ttl = doc["attributes"]["valid_until_ms"] - doc["observed_at_ms"]
            assert 14_900 <= ttl <= 15_000, ttl
    transitions = [doc["attributes"]["transition"] for doc in python_docs]
    return {"perceptions": len(python_docs), "transitions": [t for t in transitions if t != "UNCHANGED"]}


class Recorder:
    """Wraps a Python context pipeline and records each window's fused event for the C++ replay."""

    def __init__(self, inner) -> None:
        self.inner = inner
        self.model_version = getattr(inner, "model_version", None)
        self.policy_version = getattr(inner, "policy_version", None)
        self.events: list[dict] = []

    def warmup(self) -> None:
        if hasattr(self.inner, "warmup"):
            self.inner.warmup()

    def process(self, window, *, timestamp, elapsed_seconds):
        update = self.inner.process(window, timestamp=timestamp, elapsed_seconds=elapsed_seconds)
        self.events.append({"t": elapsed_seconds, "event": update.context_event})
        return update

    def write(self, path: Path) -> None:
        lines = [json.dumps({"model_version": self.model_version, "policy_version": self.policy_version})]
        lines += [json.dumps(item, ensure_ascii=False) for item in self.events]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class FakePipeline:
    """services/runtime tests FakePipeline: scripted raw label per window through the real fusion (v6 policy)."""

    model_version = "fake-audio-head"
    policy_version = "audio-policy-v6.2.1-ensemble-50-50"

    def __init__(self) -> None:
        from homemind_audio.models import Prediction
        from homemind_context_fusion.policy_config import ContextFusionPolicy
        from homemind_context_fusion.stream_fusion import FeatureStreamFusion

        self.Prediction = Prediction
        self.fusion = FeatureStreamFusion(engine=ContextFusionPolicy.from_json(DEFAULTS["fusion_policy"]).engine())

    def process(self, window, *, timestamp, elapsed_seconds):
        from homemind_audio.models import prediction_to_feature_event
        from homemind_runtime.audio.context import ContextUpdate

        rms = float(np.sqrt(np.mean(window ** 2)))
        label = "ZOOM_SPEAKER" if rms > 0.05 else ("TV_MEDIA" if rms > 0.01 else "UNKNOWN")
        prediction = self.Prediction("activity", label, 0.9 if rms > 0.01 else 0.6, self.model_version)
        speech_ratio = 0.7 if rms > 0.05 else 0.0
        event = prediction_to_feature_event(prediction, trace_id="trace-test", speech_ratio=speech_ratio,
                                            duration_seconds=elapsed_seconds, now=timestamp, simulated=True)
        result = self.fusion.ingest(event)
        return ContextUpdate(label, prediction.confidence, speech_ratio, event, result.context_event, 1.0)


def real_pipeline_available() -> bool:
    if not all(Path(DEFAULTS[key]).exists() for key in ("activity_model", "yamnet_model", "fusion_policy")):
        return False
    try:
        import tensorflow  # noqa: F401
    except Exception:  # noqa: BLE001
        return False
    return True


def python_file_run(wav_paths: list[Path], extra: dict, pipeline, *, transcriber=None, parser=None):
    settings = resolve_settings({"wav_input": [str(p) for p in wav_paths], "wav_fast": True, **extra})
    publisher = CapturePublisher()
    service = PerceptionService(settings, publisher=publisher, action_map=ActionMap(), context_pipeline=pipeline,
                                transcriber=transcriber, parser=parser, simulated=True, echo=lambda line: None)
    run_with(RuntimeConfig(), settings, publisher=publisher, service=service)
    return publisher.messages, service.summary


# ---- scenes -----------------------------------------------------------------------------------------
@pytest.fixture(scope="module")
def scenes(work) -> dict[str, list[Path]]:
    folder = work / "scenes"
    folder.mkdir(exist_ok=True)
    out: dict[str, list[Path]] = {}

    def save(name, audio):
        path = folder / f"{name}.wav"
        write_pcm16_wav(path, audio)
        return path

    out["meeting_scene"] = [save("meeting_scene", np.concatenate([noise(5.0), tone(14.0, 0.3), noise(8.0),
                                                                  tone(6.0, 0.03, 500.0), noise(12.0)]))]
    out["short"] = [save("short", tone(2.5))]
    out["two_files"] = [save("part_a", np.concatenate([noise(3.3), tone(4.1, 0.2)])),
                        save("part_b", np.concatenate([tone(0.7, 0.02), noise(9.9, seed=4)]))]
    if AIHUB.is_dir():
        import glob
        import itertools
        import wave

        meeting = next((Path(p) for p in itertools.islice(glob.iglob(str(AIHUB / "464_meeting/**/*.wav"),
                                                                     recursive=True), 1)), None)
        if meeting:
            target = folder / "aihub_meeting.wav"
            with wave.open(str(meeting), "rb") as reader, wave.open(str(target), "wb") as writer:
                reader.setpos(int(900.0 * reader.getframerate()))
                writer.setparams(reader.getparams())
                writer.writeframes(reader.readframes(int(75.0 * reader.getframerate())))
            out["aihub_meeting"] = [target]
    return out


def native_args(work: Path, paths: list[Path], extra: str = "") -> str:
    wavs = " ".join(f"--wav /t/{p.relative_to(work).as_posix()}" for p in paths)
    return f"{wavs} --fast --stdout {extra}"


# ---- 1. service-logic parity with replayed context events -------------------------------------------
@pytest.mark.parametrize("scene,pipeline_kind,extra", [
    ("meeting_scene", "fake", {}),
    ("short", "fake", {}),
    ("two_files", "fake", {"wav_offset_seconds": 0.5, "wav_max_seconds": 9.0}),
    ("meeting_scene", "fake", {"republish_seconds": 3.0, "perception_ttl_seconds": 15.0, "presence_zone": "DESK"}),
    ("aihub_meeting", "real", {}),
])
def test_context_path_replay_parity(work, scenes, scene, pipeline_kind, extra) -> None:
    if scene not in scenes:
        pytest.skip("AIHub meeting audio not available")
    if pipeline_kind == "real" and not real_pipeline_available():
        pytest.skip("Python YAMNet/TensorFlow artifacts not available")
    if pipeline_kind == "real":
        from homemind_runtime.audio.context import AudioContextPipeline

        inner = AudioContextPipeline(activity_model=DEFAULTS["activity_model"], yamnet_model=DEFAULTS["yamnet_model"],
                                     policy_path=DEFAULTS["fusion_policy"],
                                     presence_zone=extra.get("presence_zone", "UNKNOWN"), simulated=True)
    else:
        inner = FakePipeline()
    recorder = Recorder(inner)
    py_messages, py_summary = python_file_run(scenes[scene], {**extra, "wav_voice": "off"}, recorder)
    tag = f"{scene}-{pipeline_kind}-{len(REPORT)}"
    recorder.write(work / f"replay-{tag}.jsonl")
    config = write_config(work, f"cfg-{tag}.json", {**extra, "wav_voice": "off"})
    result = run_native(work, native_args(work, scenes[scene], f"--config {config} --context-replay /t/replay-{tag}.jsonl"))
    native = parse_stdout(result.stdout)
    validate_all(native)
    replay = replay_report(result.stderr)
    assert replay == {"consumed": len(recorder.events), "elapsed_mismatches": 0, "remaining": 0}, replay
    summary = summary_from(result.stderr)
    assert summary["windows"] == py_summary.windows
    assert {topic for topic, _ in native} <= {"pfn/v1/ai/perception"}
    outcome = compare_perceptions([d for t, d in py_messages if t == "pfn/v1/ai/perception"],
                                  [d for t, d in native if t == "pfn/v1/ai/perception"])
    assert [round(p["t"], 1) for p in summary["transitions"]] == [p["t"] for p in py_summary.perceptions
                                                                  if p["transition"] != "UNCHANGED"]
    REPORT[f"replay/{tag}"] = {"windows": summary["windows"], **outcome}


# ---- 3. voice decisions with fake transcripts --------------------------------------------------------
class FixedBackend:
    model_version = "fake-whisper"

    def __init__(self, texts):
        self.texts = list(texts)
        self.calls = 0

    def transcribe(self, path):
        text = self.texts[min(self.calls, len(self.texts) - 1)]
        self.calls += 1
        return SttResult(text, len(text), 0.0, self.model_version)


def native_parser_linked() -> bool:
    result = run_shell(f"{HARNESS} --help 2>&1; nm -C {HARNESS} | grep -c 'hm::VoiceParser::handle' || true")
    return result.stdout.strip().splitlines()[-1] not in {"", "0"}


VOICE_TEXTS = ["불 켜줘", "현관문 열어줘", "아 좀 덥네", "조용히 해줘"]


@pytest.mark.parametrize("mode", ["vad", "whole"])
def test_voice_decisions_with_fake_transcripts(work, scenes, mode) -> None:
    from homemind_runtime.voice import VoiceParser

    folder = work / "scenes"
    if mode == "vad":
        parts = []
        for index in range(len(VOICE_TEXTS)):
            parts += [noise(1.0, seed=index), tone(1.0, 0.3, 200.0 + 50 * index)]
        parts.append(noise(1.5))
        texts = VOICE_TEXTS
    else:
        parts = [noise(2.0), tone(8.0, 0.3), noise(3.0)]
        texts = ["아 좀 덥네"]
    wav = folder / f"voice-{mode}.wav"
    write_pcm16_wav(wav, np.concatenate(parts))
    linked = native_parser_linked()
    recorder = Recorder(FakePipeline())
    py_messages, py_summary = python_file_run(
        [wav], {"wav_voice": mode}, recorder,
        transcriber=WhisperTranscriber({}, backend=FixedBackend(texts)), parser=VoiceParser(ActionMap(), simulated=True))
    recorder.write(work / f"replay-voice-{mode}.jsonl")
    config = write_config(work, f"cfg-voice-{mode}.json", {"wav_voice": mode})
    fake = " ".join(f"--stt-fake-text '{text}'" for text in texts)
    result = run_native(work, native_args(work, [wav], f"--config {config} --context-replay /t/replay-voice-{mode}.jsonl {fake}"))
    native = parse_stdout(result.stdout)
    validate_all(native)
    py_voice = [(t, d) for t, d in py_messages if t != "pfn/v1/ai/perception"]
    cc_voice = [(t, d) for t, d in native if t != "pfn/v1/ai/perception"]
    assert [t for t, _ in cc_voice] == [t for t, _ in py_voice]
    summary = summary_from(result.stderr)
    py_kinds = [item["decision"] for item in py_summary.voice]
    cc_kinds = [item["decision"] for item in summary["voice"]]
    assert cc_kinds == py_kinds
    if linked:
        # whole mode: both sides read the context after the whole file (deterministic). vad mode: Python's --fast
        # threads race the context path, so the attached context is compared only in whole mode.
        for (topic, cc), (_, py) in zip(cc_voice, py_voice):
            if mode == "vad":
                cc, py = dict(cc), dict(py)
                cc.pop("context", None)
                py.pop("context", None)
            assert strip_volatile(cc) == strip_volatile(py), (topic, strip_volatile(cc), strip_volatile(py))
        if mode == "whole":
            assert [item.get("context") for item in summary["voice"]] == [item.get("context") for item in py_summary.voice]
    compare_perceptions([d for t, d in py_messages if t == "pfn/v1/ai/perception"],
                        [d for t, d in native if t == "pfn/v1/ai/perception"])
    REPORT[f"voice/{mode}"] = {"decisions": cc_kinds, "topics": [t for t, _ in cc_voice],
                               "parser": "libs/hm_parser" if linked else "keyword double (hm_parser not linked)"}


# ---- 2. full native pipeline vs `python -m homemind_runtime.perception` ------------------------------
def native_context_linked() -> bool:
    result = run_shell(f"nm -C {BINARY} | grep -c 'hm::AudioContextPipeline::process' || true")
    return result.stdout.strip().splitlines()[-1] not in {"", "0"}


def test_full_native_pipeline_matches_python_cli(work, scenes) -> None:
    if not (native_context_linked() and native_parser_linked()):
        pytest.skip("libs/hm_context and libs/hm_parser not linked yet (doubles only)")
    if "aihub_meeting" not in scenes or not real_pipeline_available():
        pytest.skip("real audio / Python models not available")
    wav = scenes["aihub_meeting"][0]
    config_path = work / "cfg-full.json"
    config_path.write_text(json.dumps({"simulated": True, "wav_voice": "off"}), encoding="utf-8")
    py = subprocess.run([str(PYTHON), "-m", "homemind_runtime.perception", "--config", str(config_path), "--wav",
                         str(wav), "--fast", "--stdout"], capture_output=True, text=True, encoding="utf-8",
                        timeout=1800, cwd=REPO / "services" / "runtime", env={**os.environ, "PYTHONUTF8": "1"})
    assert py.returncode == 0, py.stderr[-3000:]
    native = run_native(work, f"--config /t/cfg-full.json --wav /t/{wav.relative_to(work).as_posix()} --fast --stdout",
                        binary=BINARY)
    py_docs = [d for t, d in parse_stdout(py.stdout) if t == "pfn/v1/ai/perception"]
    cc_docs = [d for t, d in parse_stdout(native.stdout) if t == "pfn/v1/ai/perception"]
    validate_all([("pfn/v1/ai/perception", d) for d in cc_docs])
    py_summary, cc_summary = summary_from(py.stderr), summary_from(native.stderr)
    assert cc_summary["windows"] == py_summary["windows"]
    same = sum(strip_volatile(a) == strip_volatile(b) for a, b in zip(py_docs, cc_docs))
    REPORT["full_native_vs_python"] = {
        "windows": cc_summary["windows"], "python_perceptions": len(py_docs), "native_perceptions": len(cc_docs),
        "identical_documents": same,
        "python_transitions": [(p["t"], p["transition"], p["audio_context"]) for p in py_summary["transitions"]],
        "native_transitions": [(p["t"], p["transition"], p["audio_context"]) for p in cc_summary["transitions"]],
    }
    assert [(p["transition"], p["audio_context"]) for p in cc_summary["transitions"]] == \
           [(p["transition"], p["audio_context"]) for p in py_summary["transitions"]]
