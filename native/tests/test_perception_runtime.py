"""homemind-perception at run time (PC x86_64 Docker, simulated; no Raspberry Pi, no real microphone):
MQTT end to end against a mosquitto started in the container, broker restart mid-run, whisper-server down then up,
the ALSA capture path through ALSA's `file` plugin (stands in for a microphone), the push-to-talk FIFO, CLI errors,
and the runtime library dependencies (ldd).
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import REPO, run_shell, validate_topic_message  # noqa: E402
from test_perception_support import BIN, build_targets  # noqa: E402

from homemind_audio.audio import load_wav_16k_mono  # noqa: E402
from homemind_runtime.audio.stt import write_pcm16_wav  # noqa: E402

HARNESS = f"{BIN}/services/homemind-perception/homemind-perception-harness"
BINARY = f"{BIN}/services/homemind-perception/homemind-perception"
SR = 16_000
MODEL_REL = ("services/edge-ai/speech-to-text/artifacts/training/whisper-small-homemind-lora-v4/"
             "ggml-whisper-small-homemind-lora-v4-q5_0.bin")
MANIFEST = REPO / "services/edge-ai/speech-to-text/artifacts/manifests/aihub-71405-home-near-homemind-100.jsonl"
REPORT: dict[str, object] = {}


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_targets("homemind-perception homemind-perception-harness")


@pytest.fixture(scope="module")
def work(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("runtime")


@pytest.fixture(scope="module", autouse=True)
def report():
    yield
    print("\nPERCEPTION_RUNTIME_REPORT " + json.dumps(REPORT, ensure_ascii=False))


def tone(seconds, amplitude=0.3, freq=220.0):
    t = np.arange(int(seconds * SR)) / SR
    return (amplitude * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def noise(seconds, amplitude=0.0005, seed=0):
    return (amplitude * np.random.default_rng(seed).standard_normal(int(seconds * SR))).astype(np.float32)


def shell(work: Path, command: str, timeout: float = 600):
    return run_shell(command, extra_mounts=[f"{work.as_posix()}:/t"], timeout=timeout)


def messages_from_sub(text: str) -> list[tuple[str, dict]]:
    out = []
    for line in text.splitlines():
        if line.startswith("pfn/v1/"):
            topic, payload = line.split(" ", 1)
            out.append((topic, json.loads(payload)))
    return out


def config(work: Path, name: str, **extra) -> str:
    document = {"broker": {"host": "127.0.0.1", "port": 1883, "keepalive": 5}, "simulated": True, **extra}
    (work / name).write_text(json.dumps(document), encoding="utf-8")
    return f"/t/{name}"


@pytest.fixture(scope="module")
def scene(work) -> Path:
    parts = [noise(1.0), tone(1.0), noise(1.5), tone(1.0, 0.3, 330.0), noise(6.0), tone(1.0, 0.3, 440.0), noise(2.5)]
    path = work / "scene.wav"
    write_pcm16_wav(path, np.concatenate(parts))
    return path


DOUBLES = "--context-scripted --stt-fake-text '불 켜줘' --stt-fake-text '현관문 열어줘' --stt-fake-text '아 좀 덥네'"
MOSQUITTO = "mosquitto -p 1883 -d && sleep 0.3"
SUB = "(mosquitto_sub -h 127.0.0.1 -t 'pfn/v1/#' -v -q 1 >> /tmp/sub.txt &) && sleep 0.3"


def test_mqtt_end_to_end(work, scene) -> None:
    cfg = config(work, "mqtt.json", wav_voice="vad")
    stdout_run = shell(work, f"{HARNESS} --config {cfg} --wav /t/scene.wav --fast --stdout {DOUBLES}")
    assert stdout_run.returncode == 0, stdout_run.stderr[-3000:]
    expected = messages_from_sub(stdout_run.stdout)
    result = shell(work, f"{MOSQUITTO} && {SUB} && {HARNESS} --config {cfg} --wav /t/scene.wav --fast {DOUBLES} "
                         f"2>/tmp/err.txt; echo EXIT=$?; sleep 0.5; cat /tmp/sub.txt; grep -E 'mqtt' /tmp/err.txt")
    assert "EXIT=0" in result.stdout, result.stdout[-3000:]
    received = messages_from_sub(result.stdout)
    for topic, document in received:
        validate_topic_message(topic, document)
    # The context and voice threads publish independently: compare each path's sequence, not the interleaving.
    def per_path(items):
        return ([d["attributes"]["transition"] for t, d in items if t == "pfn/v1/ai/perception"],
                [t for t, _ in items if t != "pfn/v1/ai/perception"])

    assert per_path(received) == per_path(expected)
    assert "connected 127.0.0.1:1883" in result.stdout
    REPORT["mqtt_e2e"] = {"published": len(received),
                          "topics": {t: sum(1 for x, _ in received if x == t) for t in {t for t, _ in received}}}


def test_mqtt_broker_restart_mid_run(work, scene) -> None:
    cfg = config(work, "restart.json", wav_voice="vad")
    # Real-time playback (14 s). The broker goes away at 3 s and is back at 7 s; the node must keep running.
    command = (f"{MOSQUITTO} && {SUB} && ({HARNESS} --config {cfg} --wav /t/scene.wav {DOUBLES} > /tmp/out.txt 2>/tmp/err.txt; "
               f"echo EXIT=$? > /tmp/exit.txt) & sleep 3; pkill -x mosquitto; sleep 4; {MOSQUITTO} && {SUB}; "
               f"while [ ! -f /tmp/exit.txt ]; do sleep 0.5; done; sleep 1; cat /tmp/exit.txt; "
               f"echo ---SUB; cat /tmp/sub.txt; echo ---ERR; grep -E 'mqtt|ERROR' /tmp/err.txt | head -20")
    result = shell(work, command, timeout=300)
    assert "EXIT=0" in result.stdout, result.stdout[-4000:]
    sub = result.stdout.split("---SUB", 1)[1].split("---ERR", 1)[0]
    received = messages_from_sub(sub)
    for topic, document in received:
        validate_topic_message(topic, document)
    unique = {document["event_id"]: topic for topic, document in received}  # two subscribers may both deliver
    log = result.stdout.split("---ERR", 1)[1]
    expected = shell(work, f"{HARNESS} --config {cfg} --wav /t/scene.wav --fast --stdout {DOUBLES}")
    expected_topics = sorted(t for t, _ in messages_from_sub(expected.stdout))
    REPORT["mqtt_broker_restart"] = {"unique_received": len(unique), "expected": len(expected_topics),
                                     "topics": sorted(unique.values()),
                                     "log": [line for line in log.splitlines() if line.strip()][:8]}
    assert "reconnecting" in log or "disconnected" in log
    assert sorted(unique.values()) == expected_topics  # nothing lost across the outage (held, then sent)


def _speech_clips(count: int) -> list[np.ndarray]:
    if not MANIFEST.is_file():
        return []
    out = []
    for line in MANIFEST.read_text(encoding="utf-8").splitlines():
        path = Path(json.loads(line)["audio_path"].replace("\\", "/"))
        if path.is_file():
            out.append(load_wav_16k_mono(path))
        if len(out) >= count:
            break
    return out


def test_whisper_server_down_then_up(work) -> None:
    if not (REPO / "native/.deps/x64-tools/bin/whisper-server").is_file() or not (REPO / MODEL_REL).is_file():
        pytest.skip("x86_64 whisper-server or model missing")
    clips = _speech_clips(2)
    if len(clips) < 2:
        pytest.skip("AIHub clips not available")
    audio = np.concatenate([noise(1.0), clips[0], noise(9.0, seed=2), clips[1], noise(2.0, seed=5)])
    write_pcm16_wav(work / "updown.wav", audio)
    cfg = config(work, "updown.json", wav_voice="vad", whisper_server_url="http://127.0.0.1:8178",
                 whisper_cli_path="/nonexistent")
    # whisper-server starts 5 s into a real-time run: the first utterance fails (logged), the second is transcribed.
    command = (f"({HARNESS} --config {cfg} --wav /t/updown.wav --stdout --context-scripted > /tmp/out.txt 2>/tmp/err.txt; "
               f"echo EXIT=$? > /tmp/exit.txt) & sleep 6; "
               f"(/w/native/.deps/x64-tools/bin/whisper-server -m /w/{MODEL_REL} -l ko -t 8 -ng --host 127.0.0.1 "
               f"--port 8178 > /dev/null 2>&1 &); while [ ! -f /tmp/exit.txt ]; do sleep 0.5; done; cat /tmp/exit.txt; "
               f"echo ---ERR; grep -E 'whisper|voice' /tmp/err.txt; tail -1 /tmp/err.txt; echo ---OUT; cat /tmp/out.txt")
    result = shell(work, command, timeout=600)
    assert "EXIT=0" in result.stdout, result.stdout[-4000:]
    err = result.stdout.split("---ERR", 1)[1].split("---OUT", 1)[0]
    summary = json.loads([line for line in err.splitlines() if line.startswith('{"')][-1])
    out_messages = messages_from_sub(result.stdout.split("---OUT", 1)[1])
    for topic, document in out_messages:
        validate_topic_message(topic, document)
    assert "whisper failed" in err
    assert len(summary["voice"]) == 1 and summary["voice"][0]["decision"] != "empty"
    assert summary["windows"] > 10
    REPORT["whisper_down_then_up"] = {"failed_first": True, "second": summary["voice"][0],
                                      "perceptions": sum(1 for t, _ in out_messages if t == "pfn/v1/ai/perception")}


ASOUNDRC = """pcm.hmfile {
  type file
  slave.pcm "null"
  file "/dev/null"
  infile "/t/mic.raw"
  format "raw"
}
"""


@pytest.fixture(scope="module")
def alsa_input(work) -> None:
    audio = np.concatenate([noise(1.0), tone(2.0), noise(3.0)])
    (np.clip(audio, -1, 1) * 32767).astype("<i2").tofile(work / "mic.raw")
    (work / "asoundrc").write_text(ASOUNDRC, encoding="utf-8")


def test_alsa_capture_path_via_file_plugin(work, alsa_input) -> None:
    # The null slave is not clocked: the file plays as fast as it can be read (stream time runs ahead).
    cfg = config(work, "alsa.json", audio_device="hmfile", listen_mode="always", publish="stdout")
    command = (f"cp /t/asoundrc /root/.asoundrc && {HARNESS} --list-devices && "
               f"timeout --preserve-status -s TERM 3 {HARNESS} --config {cfg} {DOUBLES} > /tmp/out.txt 2>/tmp/err.txt; "
               f"echo EXIT=$?; grep -E 'capture|stopping' /tmp/err.txt; echo ---OUT; head -c 200000 /tmp/out.txt")
    result = shell(work, command)
    assert "EXIT=0" in result.stdout, result.stdout[-3000:]
    assert "hmfile" in result.stdout.split("EXIT=")[0]
    assert "microphone device=hmfile native_rate=16000" in result.stdout and "stopping" in result.stdout
    messages = messages_from_sub(result.stdout.split("---OUT", 1)[1])
    for topic, document in messages:
        validate_topic_message(topic, document)
        if topic != "pfn/v1/core/task/request":  # task requests carry no simulated flag (contract)
            assert document.get("simulated") is True
    topics = [t for t, _ in messages]
    assert "pfn/v1/core/task/request" in topics and "pfn/v1/ai/perception" in topics
    REPORT["alsa_file_plugin"] = {"messages": len(messages), "task_requests": topics.count("pfn/v1/core/task/request")}


def test_push_to_talk_fifo_on_capture_path(work, alsa_input) -> None:
    cfg = config(work, "ptt.json", audio_device="hmfile", listen_mode="push_to_talk", publish="stdout",
                 ptt_fifo="/tmp/hm-ptt", ptt_timeout_seconds=1.0)
    command = (f"cp /t/asoundrc /root/.asoundrc && ({HARNESS} --config {cfg} {DOUBLES} > /tmp/out.txt 2>/tmp/err.txt "
               f"< /dev/null &) ; sleep 1.5; test -p /tmp/hm-ptt && echo FIFO_OK; echo press > /tmp/hm-ptt; sleep 1; "
               f"echo stop > /tmp/hm-ptt; sleep 0.5; pkill -INT -f homemind-perception-harness; sleep 1; "
               f"grep -E 'FIFO|ptt' /tmp/err.txt; echo ---OUT; grep -v '^pfn/' /tmp/out.txt")
    result = shell(work, command)
    assert "FIFO_OK" in result.stdout, result.stdout
    out = result.stdout.split("---OUT", 1)[1]
    assert "Enter를 누르고 말하세요 (Ctrl+C 종료)" in out
    assert "듣는 중" in out  # the FIFO line opened an utterance
    REPORT["ptt_fifo"] = [line for line in out.splitlines() if line.strip()][:6]


def test_cli_errors_and_list_devices(work) -> None:
    result = shell(work, f"{BINARY} --help; echo EXIT=$?; {BINARY} --listen-mode sometimes; echo EXIT=$?; "
                         f"{BINARY} --config /t/missing.json --stdout; echo EXIT=$?; "
                         f"{BINARY} --list-devices; echo EXIT=$?")
    exits = [line for line in result.stdout.splitlines() if line.startswith("EXIT=")]
    assert exits == ["EXIT=0", "EXIT=2", "EXIT=1", "EXIT=0"], result.stdout + result.stderr
    assert "invalid choice: 'sometimes'" in result.stderr


def test_runtime_library_dependencies(work) -> None:
    result = shell(work, f"ldd {BINARY}; ls -la {BINARY}; strip -o /tmp/p {BINARY} && ls -la /tmp/p")
    libraries = sorted({line.split()[0] for line in result.stdout.splitlines() if ".so" in line and "=>" in line})
    REPORT["ldd"] = libraries
    sizes = [line.split()[4] for line in result.stdout.splitlines() if line.startswith("-")]
    REPORT["binary_bytes_debug_stripped"] = sizes
    assert "libasound.so.2" in libraries and "libmosquitto.so.1" in libraries
