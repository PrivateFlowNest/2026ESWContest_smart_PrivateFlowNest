"""homemind-perception live inputs (agent F, PC x86_64 Docker, simulated; no Raspberry Pi, no real camera/microphone):

1. Remote push-to-talk, pfn/v1/ai/voice/listen (contracts/pfn.v1/voice-listen.schema.json):
   - schema decisions of the C++ check == Python jsonschema (Draft 2020-12 + FormatChecker) on 100+ documents
   - MQTT end to end with mosquitto in the container: expired / invalid / duplicate requests ignored (logged without
     content), start = local push-to-talk, cancel discards the utterance, inactive outside push_to_talk
2. Camera presence into context-fusion (presence_fifo):
   - scripted PRESENCE stream + real YAMNet/fusion pipeline (--fusion-trace) replayed through the Python
     FeatureStreamFusion: identical fused events (zone changes, low confidence, duplicate, out of order, future,
     stale, invalid)
   - homemind-vision (COCO candidate model, dog.jpg, realtime) -> FIFO -> homemind-perception (AIHub meeting clip,
     real time): ACTIVITY_CONTEXT zone from vision, outdoor camera filtered, same Python replay

    $env:HM_NATIVE_BUILDER = "perception2"; $env:PYTHONUTF8 = "1"
    services/runtime/.venv/Scripts/python -m pytest native/tests/test_perception_live.py -s -p no:cacheprovider \
        --basetemp "$env:TEMP/hmpt/f1"
"""

from __future__ import annotations

import copy
import json
import os
import sys
import uuid
from pathlib import Path
from typing import Any

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import CONTRACTS, REPO, run_shell, validate_topic_message  # noqa: E402
from test_perception_support import BIN, build_targets  # noqa: E402

from homemind_runtime.audio.stt import write_pcm16_wav  # noqa: E402

HARNESS = f"{BIN}/services/homemind-perception/homemind-perception-harness"
PROBE = f"{BIN}/services/homemind-perception/homemind-perception-probe"
VISION = f"{BIN}/services/homemind-vision/homemind-vision"
SR = 16_000
AIHUB = Path(os.environ.get("HM_AIHUB_DIR", "E:/HomeMind-Datasets/raw/aihub"))
POLICY = REPO / "services/edge-ai/context-fusion/config/audio-policy-v6.3-hold15.json"  # perception default
COCO = REPO / "services/edge-ai/vision-presence/artifacts/candidates/yolox_nano_416.onnx"
DOG = REPO / "services/edge-ai/vision-presence/data/surrogate/dog.jpg"
REPORT: dict[str, Any] = {"scope": "PC x86_64 Docker (simulation, not a Raspberry Pi)"}
MOSQUITTO = "mosquitto -p 1883 -d && sleep 0.3"


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_targets("homemind-perception homemind-perception-harness homemind-perception-probe homemind-vision "
                  "homemind_perception_tests")


@pytest.fixture(scope="module")
def work(tmp_path_factory) -> Path:
    return tmp_path_factory.mktemp("live")


@pytest.fixture(scope="module", autouse=True)
def report():
    yield
    print("\nPERCEPTION_LIVE_REPORT " + json.dumps(REPORT, ensure_ascii=False))


def shell(work: Path, command: str, timeout: float = 600):
    return run_shell(command, extra_mounts=[f"{work.as_posix()}:/t"], timeout=timeout)


def tone(seconds, amplitude=0.3, freq=220.0):
    t = np.arange(int(seconds * SR)) / SR
    return (amplitude * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def noise(seconds, amplitude=0.0005, seed=0):
    return (amplitude * np.random.default_rng(seed).standard_normal(int(seconds * SR))).astype(np.float32)


def parse_messages(text: str) -> list[tuple[str, dict]]:
    out = []
    for line in text.splitlines():
        if line.startswith("pfn/v1/"):
            topic, payload = line.split(" ", 1)
            out.append((topic, json.loads(payload)))
    return out


def write_config(work: Path, name: str, **extra) -> str:
    document = {"broker": {"host": "127.0.0.1", "port": 1883, "keepalive": 5}, "simulated": True, **extra}
    (work / name).write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
    return f"/t/{name}"


def test_gtests_pass(work) -> None:
    result = shell(work, f"{BIN}/services/homemind-perception/homemind_perception_tests --gtest_brief=1 2>&1 | tail -5")
    assert "PASSED" in result.stdout and "FAILED" not in result.stdout, result.stdout
    REPORT["gtest_homemind_perception"] = result.stdout.strip().splitlines()[-1]


# ---- 1. pfn/v1/ai/voice/listen ------------------------------------------------------------------------------
def listen(**changes) -> dict:
    document = json.loads((CONTRACTS / "pfn.v1/examples/voice-listen-start.json").read_text(encoding="utf-8"))
    document["event_id"] = str(uuid.uuid4())
    document.update(changes)
    return document


def listen_variants() -> list[dict]:
    base = listen()
    variants: list[Any] = [base, [], "start", None, {}]
    for key in list(base):
        reduced = copy.deepcopy(base)
        del reduced[key]
        variants.append(reduced)
    uuids = ["5F0E0000-0000-4000-8000-0000000000A1", "{5f0e0000-0000-4000-8000-0000000000a1}",
             "urn:uuid:5f0e0000-0000-4000-8000-0000000000a1", "5f0e0000000040008000000000000a1",
             "5f0e0000-0000-4000-8000-0000000000a", "5f0e0000-0000-4000-8000-0000000000ag", "+f0e0000-0000-4000-8000-0000000000a1",
             "5f0e_000-0000-4000-8000-0000000000a1", "５f0e0000-0000-4000-8000-0000000000a1", " f0e0000-0000-4000-8000-0000000000a1",
             "5f0e0000-0000-4000-8000-0000000000a1 ", "", 5, None, "00000000-0000-0000-0000-000000000000"]
    for key in ("event_id", "trace_id"):
        for value in uuids:
            variants.append({**base, key: value})
    for key, values in {
        "schema_version": ["pfn.v1", "pfn.v2", "", 1, None],
        "source_node_id": ["NODE-RPI5-01", "NODE-RPI5-02", "NODE-RPI4-01", "node-rpi5-01", None],
        "target_node_id": ["NODE-RPI5-02", "NODE-RPI5-01", "", None],
        "client_id": ["a", "", "x" * 64, "x" * 65, "가" * 64, "가" * 65, "😀" * 64, "😀" * 65, 7, None, ["ui"]],
        "action": ["start", "cancel", "stop", "START", "", None, 1],
        "sent_at_ms": [0, -1, 1.0, 1.5, 1792560000000, 1792560000000.0, "1", True, None, 2 ** 63, -0.0],
        "expires_at_ms": [0, -5, 3.0, 3.25, 10 ** 20, "3", False, None, 1e300],
        "simulated": [True, False, 0, 1, "true", None],
    }.items():
        for value in values:
            variants.append({**base, key: value})
    variants += [{**base, "extra": 1}, {**base, "text": "불 켜줘"}, {**base, "audio": "AAAA"},
                 {**base, "simulated": True, "client_id": "homemind-ui-7inch"}]
    return variants


def test_voice_listen_schema_parity() -> None:
    from jsonschema import Draft202012Validator, FormatChecker

    schema = json.loads((CONTRACTS / "pfn.v1/voice-listen.schema.json").read_text(encoding="utf-8"))
    validator = Draft202012Validator(schema, format_checker=FormatChecker())
    variants = listen_variants()
    requests = [json.dumps({"op": "voice_listen", "document": doc}, ensure_ascii=False) for doc in variants]
    result = run_shell(PROBE, stdin="\n".join(requests) + "\n")
    assert result.returncode == 0, result.stderr
    answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(answers) == len(variants)
    mismatches, valid = [], 0
    for doc, answer in zip(variants, answers):
        python_valid = not list(validator.iter_errors(doc))
        native_valid = answer["problem"] == ""
        valid += python_valid
        if python_valid != native_valid:
            mismatches.append((doc, answer))
        if not native_valid:  # the reason names a field and a rule, never the value
            for value in (doc.values() if isinstance(doc, dict) else []):
                if isinstance(value, str) and len(value) > 3:
                    assert value not in answer["problem"], answer
    REPORT["voice_listen_schema"] = {"documents": len(variants), "valid": valid, "mismatches": len(mismatches)}
    assert not mismatches, mismatches[:5]


def test_voice_listen_over_mqtt(work) -> None:
    """Real-time push-to-talk file playback (--voice ptt) driven only by MQTT presses from 'the 7-inch UI'."""

    audio = np.concatenate([noise(3.5), tone(1.0), noise(4.0), tone(1.0, 0.3, 330.0), noise(4.5, seed=3)])
    write_pcm16_wav(work / "ptt.wav", audio)  # tones at 3.5-4.5 s and 8.5-9.5 s, 14 s in all
    cfg = write_config(work, "ptt.json", wav_voice="ptt", ptt_timeout_seconds=6.0)

    def pub(document: dict, name: str) -> str:
        (work / name).write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        return f"mosquitto_pub -h 127.0.0.1 -q 1 -t pfn/v1/ai/voice/listen -f /t/{name}"

    now = "$(($(date +%s%N)/1000000))"
    first = str(uuid.uuid4())
    stale = listen(sent_at_ms=1792560000000, expires_at_ms=1792560003000)  # long expired
    broken = listen(action="toggle")
    # Fresh timestamps are filled in inside the container right before publishing.
    script = (
        f"{MOSQUITTO} && (mosquitto_sub -h 127.0.0.1 -t 'pfn/v1/#' -v -q 1 > /tmp/sub.txt &) && sleep 0.3 && "
        f"({HARNESS} --config {cfg} --wav /t/ptt.wav --stt-fake-text '불 켜줘' --context-scripted "
        f"> /tmp/out.txt 2> /tmp/err.txt; echo EXIT=$? > /tmp/exit.txt) & sleep 1.0; "
        f"{pub(stale, 'stale.json')}; {pub(broken, 'broken.json')}; sleep 1.0; "
        f"fresh() {{ sed -e \"s/\\\"sent_at_ms\\\": [0-9]*/\\\"sent_at_ms\\\": {now}/\" "
        f"-e \"s/\\\"expires_at_ms\\\": [0-9]*/\\\"expires_at_ms\\\": $(( {now} + 3000 ))/\" /t/$1 > /tmp/$1; "
        f"mosquitto_pub -h 127.0.0.1 -q 1 -t pfn/v1/ai/voice/listen -f /tmp/$1; }}; "
        f"fresh start1.json; sleep 0.2; fresh start1.json; sleep 4.3; "   # 2.0 s: start; duplicate; tone 3.5-4.5 s
        f"fresh start2.json; sleep 0.5; fresh cancel2.json; "             # 6.5 s: start, 7.0 s: cancel (tone 8.5 s ignored)
        f"while [ ! -f /tmp/exit.txt ]; do sleep 0.3; done; sleep 0.5; cat /tmp/exit.txt; "
        f"echo ---SUB; cat /tmp/sub.txt; echo ---OUT; cat /tmp/out.txt; echo ---ERR; "
        f"grep -E 'voice/listen|remote push-to-talk|perception.*(FIFO|listening)' /tmp/err.txt; tail -1 /tmp/err.txt")
    (work / "start1.json").write_text(json.dumps(listen(event_id=first), ensure_ascii=False), encoding="utf-8")
    (work / "start2.json").write_text(json.dumps(listen(), ensure_ascii=False), encoding="utf-8")
    (work / "cancel2.json").write_text(json.dumps(listen(action="cancel"), ensure_ascii=False), encoding="utf-8")
    result = shell(work, script, timeout=300)
    assert "EXIT=0" in result.stdout, result.stdout[-4000:] + result.stderr[-2000:]
    sub = result.stdout.split("---SUB", 1)[1].split("---OUT", 1)[0]
    out = result.stdout.split("---OUT", 1)[1].split("---ERR", 1)[0]
    err = result.stdout.split("---ERR", 1)[1]
    received = parse_messages(sub)
    for topic, document in received:
        if topic != "pfn/v1/ai/voice/listen":
            validate_topic_message(topic, document)
    tasks = [d for t, d in received if t == "pfn/v1/core/task/request"]
    summary = json.loads(err.strip().splitlines()[-1])
    REPORT["voice_listen_mqtt"] = {
        "task_requests": len(tasks), "voice": summary["voice"],
        "log": [line.split(" ", 2)[-1] for line in err.splitlines() if "voice/listen" in line or "remote" in line],
    }
    assert "ignored pfn/v1/ai/voice/listen: expired" in err
    assert "ignored pfn/v1/ai/voice/listen: invalid (action:" in err
    assert "ignored pfn/v1/ai/voice/listen: duplicate event" in err
    assert err.count("remote push-to-talk start") == 2 and err.count("remote push-to-talk cancel") == 1
    assert "remote push-to-talk: listening on pfn/v1/ai/voice/listen" in err
    assert "toggle" not in err and "homemind-ui-7inch" not in err  # ignored requests are not quoted
    assert out.count("듣는 중") == 2 and "듣기를 취소했습니다" in out
    assert len(tasks) == 1 and tasks[0]["parameters"]["ai_device"] == "LIGHT"  # only the first press reached whisper
    assert tasks[0]["trace_id"] == listen()["trace_id"]  # the UI press trace is the task request trace
    assert len(summary["voice"]) == 1


def test_voice_listen_inactive_outside_push_to_talk(work) -> None:
    audio = np.concatenate([noise(1.0), tone(1.0), noise(2.0)])
    write_pcm16_wav(work / "vad.wav", audio)
    cfg = write_config(work, "vad.json", wav_voice="vad")
    (work / "start-inactive.json").write_text(json.dumps(listen(expires_at_ms=2 ** 62)), encoding="utf-8")
    script = (f"{MOSQUITTO} && ({HARNESS} --config {cfg} --wav /t/vad.wav --stt-fake-text '불 켜줘' --context-scripted "
              f"> /tmp/out.txt 2> /tmp/err.txt; echo EXIT=$? > /tmp/exit.txt) & sleep 1; "
              f"mosquitto_pub -h 127.0.0.1 -q 1 -t pfn/v1/ai/voice/listen -f /t/start-inactive.json; "
              f"while [ ! -f /tmp/exit.txt ]; do sleep 0.3; done; cat /tmp/exit.txt; grep -c 'voice/listen\\|remote' /tmp/err.txt")
    result = shell(work, script, timeout=120)
    assert "EXIT=0" in result.stdout
    assert result.stdout.strip().splitlines()[-1] == "0"  # not subscribed: nothing received, nothing logged


# ---- 2. camera presence into context-fusion ---------------------------------------------------------------
def python_fusion():
    from homemind_context_fusion.policy_config import ContextFusionPolicy
    from homemind_context_fusion.stream_fusion import FeatureStreamFusion

    return FeatureStreamFusion(engine=ContextFusionPolicy.from_json(POLICY).engine())


def replay_trace_through_python(trace_path: Path) -> dict[str, Any]:
    fusion = python_fusion()
    windows = presence_events = identical = 0
    reasons: dict[str, int] = {}
    statuses: dict[str, int] = {}
    zones: list[str] = []
    for line in trace_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        item = json.loads(line)
        for presence in item["presence"]:
            result = fusion.ingest(presence["event"])
            assert result.reason == presence["result"], (item["t"], result.reason, presence["result"])
            reasons[result.reason] = reasons.get(result.reason, 0) + 1
            presence_events += 1
        result = fusion.ingest(item["feature"])
        python = dict(result.context_event)
        native = dict(item["context"])
        assert python.pop("event_id").startswith("ctx-") and native.pop("event_id").startswith("ctx-")
        assert native == python, (item["t"], native, python)
        identical += 1
        windows += 1
        status = result.input_status["PRESENCE"]
        statuses[status] = statuses.get(status, 0) + 1
        zones.append(native["presence_zone"])
    return {"windows": windows, "identical_fused_events": identical, "presence_events": presence_events,
            "ingest_results": reasons, "presence_status": statuses,
            "zones": [z for i, z in enumerate(zones) if i == 0 or zones[i - 1] != z]}


@pytest.fixture(scope="module")
def meeting(work) -> tuple[Path, str]:
    """AIHub 464 meeting (75 s from 900 s, like test_perception_e2e) or a synthetic stand-in."""

    import glob
    import itertools
    import wave

    target = work / "meeting.wav"
    source = None
    if AIHUB.is_dir():
        source = next((Path(p) for p in itertools.islice(glob.iglob(str(AIHUB / "464_meeting/**/*.wav"),
                                                                    recursive=True), 1)), None)
    if source:
        with wave.open(str(source), "rb") as reader, wave.open(str(target), "wb") as writer:
            reader.setpos(int(900.0 * reader.getframerate()))
            writer.setparams(reader.getparams())
            writer.writeframes(reader.readframes(int(75.0 * reader.getframerate())))
        return target, "aihub_464_meeting_75s"
    write_pcm16_wav(target, np.concatenate([noise(5.0), tone(30.0, 0.3), noise(20.0), tone(20.0, 0.05, 500.0)]))
    return target, "synthetic_75s"


PRESENCE_SCRIPT = [
    {"at": 0, "zone": "DESK", "confidence": 0.9, "ttl": 15},
    {"at": 5.5, "zone": "DESK", "confidence": 0.92, "ttl": 15, "offset": 0.7},   # newer than the 6 s window: held
    {"at": 12, "zone": "BED", "confidence": 0.95, "ttl": 15},
    {"at": 18, "zone": "DESK", "confidence": 0.3, "ttl": 15},                    # low confidence -> UNKNOWN
    {"at": 22, "repeat_previous": True},                                          # same event_id -> duplicate
    {"at": 26, "zone": "COMMON", "confidence": 0.8, "ttl": 15},
    {"at": 27, "zone": "BED", "confidence": 0.8, "ttl": 15, "offset": -5},       # older than COMMON -> out_of_order
    {"at": 30, "zone": "BATHROOM", "confidence": 0.9, "ttl": 3},                 # stale from 33 s on
    {"at": 40, "raw": {"event_type": "PRESENCE", "payload": {"presence_zone": "KITCHEN"}}},  # rejected
    {"at": 45, "zone": "DESK", "confidence": 0.85, "ttl": 15, "simulated": False},
    {"at": 62, "zone": "OUTDOOR", "confidence": 0.7, "ttl": 15},
]


def test_scripted_presence_fusion_matches_python(work, meeting) -> None:
    wav, kind = meeting
    (work / "presence.jsonl").write_text("\n".join(json.dumps(item) for item in PRESENCE_SCRIPT) + "\n",
                                         encoding="utf-8")
    cfg = write_config(work, "fusion.json", wav_voice="off", republish_seconds=3.0)
    result = shell(work, f"{HARNESS} --config {cfg} --wav /t/{wav.name} --fast --stdout "
                         f"--presence-replay /t/presence.jsonl --fusion-trace /t/trace-scripted.jsonl", timeout=900)
    assert result.returncode == 0, result.stderr[-3000:]
    replay = replay_trace_through_python(work / "trace-scripted.jsonl")
    perceptions = [d for t, d in parse_messages(result.stdout) if t == "pfn/v1/ai/perception"]
    for document in perceptions:
        validate_topic_message("pfn/v1/ai/perception", document)
    REPORT["presence_fusion_scripted"] = {"audio": kind, **replay,
                                          "published_zones": sorted({d["zone"] for d in perceptions})}
    assert replay["windows"] == 72 and replay["identical_fused_events"] == 72
    assert replay["ingest_results"] == {"buffered": 8, "duplicate": 1, "out_of_order": 1}
    assert {"fresh", "low_confidence", "stale"} <= set(replay["presence_status"])
    assert "future" not in replay["presence_status"]  # a newer presence waits for its window instead
    assert replay["zones"][:4] == ["DESK", "BED", "UNKNOWN", "COMMON"]
    assert {"desk", "bed", "living", "bathroom", "outdoor", "unknown"} <= {d["zone"] for d in perceptions}  # COMMON = living
    assert "presence item rejected" in result.stderr


def test_vision_fifo_to_perception_end_to_end(work, meeting) -> None:
    """homemind-vision (COCO candidate, dog.jpg looped at 5 fps) -> /tmp/hm/presence -> homemind-perception."""

    if not COCO.is_file() or not DOG.is_file():
        pytest.skip("COCO candidate model or sample image missing")
    wav, kind = meeting
    (work / "zones.desk.json").write_text(json.dumps({
        "camera_id": "CAM-INDOOR-01", "note": "test: whole frame is the desk",
        "zones": {"DESK": [[0, 0], [1, 0], [1, 1], [0, 1]]}}), encoding="utf-8")
    image = {"type": "images", "path": "/w/" + DOG.relative_to(REPO).as_posix(), "loop": True}
    vision = {"broker": {"host": "127.0.0.1", "port": 1883}, "perception_node_id": "NODE-RPI5-02", "simulated": True,
              "vision": {"model_path": "/w/" + COCO.relative_to(REPO).as_posix(),
                         "model_version": "yolox-nano-416-coco-candidate", "threads": 2,
                         "figure_class_ids": [16, 1, 2], "interval_ms": 200, "presence_fifo": "/tmp/hm/presence",
                         "cameras": [
                             {"camera_id": "CAM-INDOOR-01", "zones": "/t/zones.desk.json", "source": image},
                             {"camera_id": "CAM-OUTDOOR-01",
                              "zones": "/w/native/services/homemind-vision/config/zones.outdoor.example.json",
                              "source": image}]}}
    (work / "vision.json").write_text(json.dumps(vision), encoding="utf-8")
    cfg = write_config(work, "e2e.json", wav_voice="off", presence_fifo="/tmp/hm/presence", presence_zone="BED")
    script = (
        "mkdir -p /tmp/hm && "
        f"({HARNESS} --config {cfg} --wav /t/{wav.name} --seconds 24 --stdout --fusion-trace /t/trace-vision.jsonl "
        f"> /tmp/perception.txt 2> /tmp/perception.err; echo EXIT=$? > /tmp/exit.txt) & sleep 0.5; "
        f"({VISION} --config /t/vision.json --stdout --realtime > /tmp/vision.txt 2> /tmp/vision.err &); "
        "while [ ! -f /tmp/exit.txt ]; do sleep 0.5; done; pkill -TERM -x homemind-vision; sleep 0.5; "
        "cat /tmp/exit.txt; echo ---VISION; cat /tmp/vision.txt; echo ---PERCEPTION; cat /tmp/perception.txt; "
        "echo ---LOG; grep -E 'presence|override' /tmp/perception.err /tmp/vision.err; tail -1 /tmp/perception.err")
    result = shell(work, script, timeout=300)
    assert "EXIT=0" in result.stdout, result.stdout[-4000:]
    vision_out = result.stdout.split("---VISION", 1)[1].split("---PERCEPTION", 1)[0]
    perception_out = result.stdout.split("---PERCEPTION", 1)[1].split("---LOG", 1)[0]
    log = result.stdout.split("---LOG", 1)[1]
    figure = [d for t, d in parse_messages(vision_out) if t == "pfn/v1/ai/perception"]
    activity = [d for t, d in parse_messages(perception_out) if t == "pfn/v1/ai/perception"]
    for document in figure + activity:
        validate_topic_message("pfn/v1/ai/perception", document)
    replay = replay_trace_through_python(work / "trace-vision.jsonl")
    summary = json.loads(log.strip().splitlines()[-1])
    indoor = [d for d in figure if d["source_device_id"] == "CAM-INDOOR-01"]
    outdoor = [d for d in figure if d["source_device_id"] == "CAM-OUTDOOR-01"]
    REPORT["vision_to_perception"] = {
        "audio": kind, "figure_presence": {"indoor": [(d["zone"], d["attributes"]["figure_count"]) for d in indoor],
                                           "outdoor": [d["zone"] for d in outdoor]},
        "presence_counts": summary.get("presence"), "transitions": summary["transitions"],
        "activity_context": [(d["attributes"]["transition"], d["attributes"]["audio_context"], d["zone"])
                             for d in activity], **replay}
    assert indoor and indoor[0]["zone"] == "desk" and outdoor and outdoor[0]["zone"] == "outdoor"
    assert summary["presence"]["accepted"] == len(indoor) and summary["presence"]["rejected"] == 0  # outdoor filtered
    assert "presence_zone override ignored" in log
    assert activity and all(d["zone"] == "desk" for d in activity), [d["zone"] for d in activity]
    assert replay["identical_fused_events"] == replay["windows"] >= 18
    assert replay["zones"] == ["DESK"] and replay["presence_status"] == {"fresh": replay["windows"]}
