"""homemind-llm (C++, NODE-RPI5-01) vs services/runtime llm_node.py LlmNode (Python reference), plus MQTT end to end.

PC x86_64 Docker only (simulation, fake llama-server, no model): not a Raspberry Pi result.
  - llm/request schema check (pfn.validate) and jsonschema "uuid" format: same accept/reject
  - LlmNode.handle parity through `homemind-llm --once-stdin` against the same scripted fake llama-server
    (statuses, origins, task requests, results modulo ids/timestamps; llama-server request bodies byte for byte)
  - MQTT: mosquitto + homemind-llm inside the container: expired -> expired result, model output -> task request +
    result, transcript text never in logs or results
"""

from __future__ import annotations

import copy
import json
import random
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guard_support import NL, OWN_BUILD, FakeLlama, build_own, probe, shell  # noqa: E402
from hmnative import CONTRACTS, validate_topic_message  # noqa: E402
from test_guard_parity import QUIET_MEETING, SLEEPING, _proposal_json  # noqa: E402

from homemind_exaone.client import ExaoneClient  # noqa: E402
from homemind_runtime import pfn  # noqa: E402
from homemind_runtime.action_map import ActionMap  # noqa: E402
from homemind_runtime.llm_node import LlmNode  # noqa: E402

LLM = f"{OWN_BUILD}/services/homemind-llm/homemind-llm"
GUARD_PROBE = "libs/hm_guard/hm_guard_probe"
MODEL = "exaone-1.2b-homemind-v1"
REPORT: dict[str, Any] = {}
MARKER = "비밀문장마커"


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_own("homemind-llm homemind_llm_tests hm_guard_probe")
    yield
    out = Path(tempfile.gettempdir()) / "hm_llm_node.json"
    out.write_text(json.dumps(REPORT, ensure_ascii=False, indent=1), encoding="utf-8")
    print("\nLLM NODE", json.dumps(REPORT, ensure_ascii=False, indent=1))


def _request(text: str, *, context: dict[str, Any] | None = None, ttl_ms: int = 60000) -> dict[str, Any]:
    return pfn.llm_request(text=text, utterance_id=str(uuid.uuid4()), stt_model_version="whisper-test",
                           stt_confidence=0.8, parser_result="NO_MATCH", context=context, ttl_ms=ttl_ms, simulated=True)


def _config(base_url: str, **broker: Any) -> str:
    return json.dumps({"broker": {"host": "127.0.0.1", "port": 1883, "username": None, "password_file": None, **broker},
                       "llm_node_id": "NODE-RPI5-01", "llm_base_url": base_url, "llm_model": MODEL, "simulated": True,
                       "contracts_dir": "/w/contracts"}, ensure_ascii=False)


def test_unit_tests_pass() -> None:
    result = shell(f"{OWN_BUILD}/services/homemind-llm/homemind_llm_tests --gtest_brief=1")
    assert result.returncode == 0, result.stdout[-3000:]


# ---------------------------------------------------------------------------------------------- schema checks
UUIDS = [str(uuid.uuid4()), str(uuid.uuid4()).upper(), "{" + str(uuid.uuid4()) + "}", "urn:uuid:" + str(uuid.uuid4()),
         uuid.uuid4().hex, str(uuid.uuid4()) + "-", "-" + str(uuid.uuid4()), "12345678-1234-1234-1234-1234567890ab",
         "12345678-1234-1234-1234-1234567890a_", "1234567_-1234-1234-1234-1234567890ab", "12345678-1234-1234-1234-12345678 0ab",
         "12345678-1234-1234-1234-1234567890ag", "", "x", "12345678-1234-1234-1234", "0x345678-1234-1234-1234-1234567890ab",
         "12345678-1234-1234-1234-1234567890ab\n", "١٢٣٤٥٦٧٨-1234-1234-1234-1234567890ab", "12345678-1234-1234-1234-1234-567890ab",
         "+2345678-1234-1234-1234-1234567890ab", "12345678-1234-1234-1234-1234567890abcd", "1234567-81234-1234-1234-1234567890ab"]


def test_jsonschema_uuid_format_matches() -> None:
    from jsonschema import FormatChecker

    checker = FormatChecker()
    answers = probe(GUARD_PROBE, [{"op": "uuid", "value": value} for value in UUIDS])
    diffs = [(v, checker.conforms(v, "uuid"), a) for v, a in zip(UUIDS, answers) if a.get("valid") != checker.conforms(v, "uuid")]
    REPORT["uuid_format"] = f"{len(UUIDS) - len(diffs)}/{len(UUIDS)}"
    assert not diffs, diffs


def _request_mutations() -> list[Any]:
    base = _request("아 좀 덥네", context=QUIET_MEETING)
    out: list[Any] = [base, _request("불 켜줘"), [], "x", None]
    weird = [None, True, 0, 1, -1, 5.0, 5.5, 1.5, "", "x", "가" * 500, "가" * 501, [], {}, "pfn.v1", "NODE-RPI5-02",
             "ko", "NO_MATCH", str(uuid.uuid4())] + UUIDS[:8]
    for key in list(base):
        out.append({k: v for k, v in base.items() if k != key})
        for value in weird:
            out.append(dict(base, **{key: value}))
    for section in ("transcript", "parser", "context"):
        for key in list(base[section]):
            out.append(dict(base, **{section: {k: v for k, v in base[section].items() if k != key}}))
            for value in weird:
                out.append(dict(base, **{section: dict(base[section], **{key: value})}))
        out.append(dict(base, **{section: dict(base[section], extra=1)}))
    out.append(dict(base, extra=1))
    out.append(dict(base, simulated=False))
    return out


def test_llm_request_schema_check_matches() -> None:
    requests = _request_mutations()
    answers = probe(GUARD_PROBE, [{"op": "llm_request_check", "request": r} for r in requests])
    diffs, rejected = [], 0
    for request, answer in zip(requests, answers):
        try:
            pfn.validate(request, "llm-request.schema.json")
            expected_ok = True
        except Exception:  # noqa: BLE001 - jsonschema raises ValueError; non-objects too
            expected_ok = False
        rejected += not expected_ok
        if expected_ok != (answer.get("problem") == ""):
            diffs.append((request, expected_ok, answer))
    REPORT["llm_request_schema"] = {"identical": f"{len(requests) - len(diffs)}/{len(requests)}", "rejected": rejected}
    assert not diffs, json.dumps(diffs[:5], ensure_ascii=False, default=str)


# ---------------------------------------------------------------------------------------------- LlmNode parity
FILLERS = ["아", "음", "어", "그냥", "저기", "근데", "흠", "에휴", "아이고", "있잖아", "참", "후"]
REPLY_SETS: list[list[dict[str, Any]]] = [
    [{"content": _proposal_json(device="AIR_CONDITIONER")}],
    [{"content": _proposal_json(device="VENTILATION_FAN")}],
    [{"content": _proposal_json(device="WINDOW", action="OPEN")}],
    [{"content": _proposal_json(device="ENTRY_DOOR", action="OPEN")}],
    [{"content": _proposal_json(device="LIGHT", action="SET_BRIGHTNESS", parameters={"brightness": 30})}],
    [{"content": _proposal_json(device="TV", action="SET_VOLUME", parameters={"volume": 10})}],
    [{"content": _proposal_json(intent="DEVICE_QUERY", device="WASHER", action="QUERY_STATUS")}],
    [{"content": _proposal_json(intent="CONTEXT_SUGGESTION", device="ALL_QUIET_DEVICES", action="SET_QUIET_MODE")}],
    [{"content": _proposal_json(device="ALL_QUIET_DEVICES", action="RESUME", parameters={"reason": "회의 상황 종료"})}],
    [{"content": _proposal_json(intent="NO_ACTION", device=None, action=None)}],
    [{"content": _proposal_json(intent="CLARIFY", device=None, action=None, ambiguous=True,
                                safety_flags=["AMBIGUOUS_TARGET"], clarification_question="어느 기기를 말씀하셨나요?")}],
    [{"content": "garbage"}, {"content": "```json {}```"}],
    [{"content": _proposal_json(device="ROBOT_VACUUM", action="START")}],
    [{"status": 503}],
    [{"close": True}],
    [{"content": "not json"}, {"content": _proposal_json(device="AIR_CONDITIONER", action="TURN_OFF")}],
]
TEXTS = ["좀 덥네", "방이 눅눅해", "어둡다", "회의 시작해", "너무 시끄러워", "답답해"]
GUARD_TEXTS = ["거실 불 켜줘", "거실 불은 켜지 마", "창문 열어 줘", "현관문 닫아줘", "TV 볼륨 20으로 맞춰줘", "로봇청소기 돌려줘",
               "가스 냄새가 나", "에어컨 26도로 맞춰줘", "불 켜져 있어?", "조용히 해줘", "세탁기 켜줘"]


def _llm_cases() -> list[tuple[dict[str, Any], list[dict[str, Any]]]]:
    rng = random.Random(23)
    cases, used = [], set()

    def unique(text: str) -> str:
        while True:
            candidate = f"{rng.choice(FILLERS)} {rng.choice(FILLERS)} {text}"
            if candidate not in used:
                used.add(candidate)
                return candidate

    for replies in REPLY_SETS:
        for text in TEXTS[:3]:
            context = rng.choice([None, QUIET_MEETING, SLEEPING])
            cases.append((_request(unique(text), context=context), replies))
    for text in GUARD_TEXTS:
        for replies in (REPLY_SETS[0], REPLY_SETS[9], REPLY_SETS[14]):
            cases.append((_request(text if not used.intersection({text}) else unique(text)), replies))
            used.add(text)
    cases.append((_request(unique("좀 덥네"), ttl_ms=-1), REPLY_SETS[0]))  # expired
    cases.append((dict(_request(unique("좀 덥네")), extra=1), REPLY_SETS[0]))  # schema rejects
    cases.append((_request(" 　 "), REPLY_SETS[0]))  # empty after strip -> ValueError
    return cases


def _normalize(messages: list[list[Any]]) -> list[Any]:
    out = []
    task_ids = [doc["task_id"] for topic, doc in messages if topic == pfn.TOPICS["task_request"]]
    for topic, doc in messages:
        doc = copy.deepcopy(doc)
        for key in ("event_id", "task_id"):
            if doc.get(key):
                uuid.UUID(doc[key])
                doc[key] = "<id>"
        for key in ("completed_at_ms", "requested_at_ms"):
            if key in doc:
                doc[key] = "<t>"
        if topic == pfn.TOPICS["llm_result"]:
            if doc["model"]["origin"] not in ("guard", "none"):
                doc["model"]["latency_ms"] = "<latency>"
            if doc["status"] == "task_requested":
                assert messages[0][1]["task_id"] in task_ids
        out.append([topic, doc])
    return out


def test_llm_node_parity_with_fake_llama_server() -> None:
    cases = _llm_cases()
    with FakeLlama() as server:
        server.scripts["py-node"] = {c[0]["transcript"]["text"].strip(): c[1] for c in cases if isinstance(c[0], dict)}
        server.scripts["cpp-node"] = server.scripts["py-node"]
        node = LlmNode(ExaoneClient(base_url=server.url("py-node", container=False), model=MODEL, timeout_seconds=30.0),
                       ActionMap(CONTRACTS), node_id="NODE-RPI5-01", simulated=True)
        expected = []
        for request, _ in cases:
            try:
                expected.append({"messages": [list(m) for m in node.handle(copy.deepcopy(request))]})
            except ValueError as error:
                expected.append({"error": str(error)})
        command = (f"cat > /tmp/llm.json <<'EOF'{NL}{_config(server.url('cpp-node', container=True))}{NL}EOF{NL}"
                   f"{LLM} --config /tmp/llm.json --once-stdin")
        stdin = NL.join(json.dumps(request, ensure_ascii=True) for request, _ in cases) + NL
        result = shell(command, stdin=stdin, timeout=600)
        assert result.returncode == 0, result.stderr[-3000:]
        answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        py_bodies, cpp_bodies = server.bodies("py-node"), server.bodies("cpp-node")
    assert len(answers) == len(cases)
    assert MARKER not in result.stderr
    diffs, statuses = [], {}
    for (request, replies), py_out, cpp_out in zip(cases, expected, answers):
        if "error" in py_out or "error" in cpp_out:
            if ("error" in py_out) != ("error" in cpp_out):
                diffs.append((request, replies, py_out, cpp_out))
            continue
        for topic, document in cpp_out["messages"]:
            validate_topic_message(topic, document)
        if _normalize(py_out["messages"]) != _normalize(cpp_out["messages"]):
            diffs.append((request["transcript"]["text"], replies, py_out, cpp_out))
        status = py_out["messages"][-1][1]["status"]
        statuses[status] = statuses.get(status, 0) + 1
    body_diffs = [u for u in set(py_bodies) | set(cpp_bodies) if py_bodies.get(u) != cpp_bodies.get(u)]
    REPORT["llm_node"] = {"identical": f"{len(cases) - len(diffs)}/{len(cases)}", "statuses": statuses,
                          "llama_request_bodies_identical": f"{sum(len(v) for v in py_bodies.values())} bodies, "
                                                            f"{len(body_diffs)} utterances differ"}
    assert not diffs, json.dumps(diffs[:4], ensure_ascii=False, indent=1, default=str)
    assert not body_diffs, body_diffs[:5]


# ---------------------------------------------------------------------------------------------- MQTT end to end
def test_mqtt_end_to_end_with_mosquitto_in_container() -> None:
    secret = f"{MARKER} 좀 덥네"
    long_text = (MARKER + " 아") * 80  # > 500 characters: schema rejects it
    requests = [
        _request("어휴 좀 덥네", ttl_ms=-1),                                # expired
        _request("에휴 좀 덥네", context=QUIET_MEETING),                    # model -> task + result
        _request("거실 불은 켜지 마"),                                       # guard -> no_action
        _request("흠 좀 덥네"),                                             # model down -> model_unavailable
        dict(_request(secret), extra=True),                               # schema rejects -> nothing
        dict(_request("아 좀 덥네"), transcript={"text": long_text, "language": "ko", "stt_model_version": "x",
                                               "stt_confidence": None}),  # too long -> nothing
        _request(secret),                                                 # model -> no_action
    ]
    script = {"에휴 좀 덥네": [{"content": _proposal_json(device="AIR_CONDITIONER")}],
              "흠 좀 덥네": [{"close": True}],
              secret: [{"content": _proposal_json(intent="NO_ACTION", device=None, action=None)}]}
    # Python reference raises for the too-long transcript with the transcript text in the message (logged by Bus)
    try:
        pfn.validate(requests[5], "llm-request.schema.json")
        python_leaks = False
    except ValueError as error:
        python_leaks = MARKER in str(error)
    with FakeLlama() as server:
        server.scripts["mqtt"] = script
        command = NL.join([
            "set -u",
            "cat > /tmp/requests.jsonl",
            f"cat > /tmp/llm.json <<'EOF'{NL}{_config(server.url('mqtt', container=True))}{NL}EOF",
            "mosquitto -p 1883 -d",
            "sleep 0.5",
            f"{LLM} --config /tmp/llm.json --log-level DEBUG 2> /tmp/llm.log &",
            "LLM_PID=$!",
            "for i in $(seq 1 50); do grep -q connected /tmp/llm.log && break; sleep 0.2; done",
            "mosquitto_sub -h 127.0.0.1 -t pfn/v1/ai/llm/result -t pfn/v1/core/task/request -v -C 6 -W 40 > /tmp/sub.txt &",
            "SUB_PID=$!",
            "sleep 1",
            "mosquitto_pub -h 127.0.0.1 -t pfn/v1/ai/llm/request -q 1 -l < /tmp/requests.jsonl",
            "wait $SUB_PID",
            "sleep 0.5",
            "kill -TERM $LLM_PID; wait $LLM_PID; echo EXIT=$?",
            "echo ===SUB; cat /tmp/sub.txt; echo ===LOG; cat /tmp/llm.log",
        ])
        stdin = NL.join(json.dumps(r, ensure_ascii=False) for r in requests) + NL
        result = shell(command, stdin=stdin, timeout=180)
    output = result.stdout
    assert "===SUB" in output, output + result.stderr
    sub = output.split("===SUB", 1)[1].split("===LOG", 1)[0].strip().splitlines()
    log = output.split("===LOG", 1)[1]
    received = []
    for line in sub:
        topic, payload = line.split(" ", 1)
        document = json.loads(payload)
        validate_topic_message(topic, document)
        received.append((topic, document))
    by_trace: dict[str, list[tuple[str, dict[str, Any]]]] = {}
    for topic, document in received:
        by_trace.setdefault(document["trace_id"], []).append((topic, document))

    def statuses(index: int) -> list[str]:
        return [d.get("status", d.get("intent")) for _, d in by_trace.get(requests[index]["trace_id"], [])]

    assert statuses(0) == ["expired"]
    assert statuses(1) == ["device_control", "task_requested"]
    task, result_doc = (d for _, d in by_trace[requests[1]["trace_id"]])
    assert task["parameters"]["origin"] == "llm" and task["target_device"] == "LED-AIRCON-01"
    assert result_doc["task_id"] == task["task_id"] and result_doc["model"]["origin"] == "model"
    assert statuses(2) == ["no_action"]
    assert statuses(3) == ["model_unavailable"]
    assert statuses(4) == [] and statuses(5) == []
    assert statuses(6) == ["no_action"]
    assert len(received) == 6
    assert MARKER not in output, "transcript text leaked into MQTT results or logs"
    assert "덥네" not in log and "켜지 마" not in log
    assert log.count("request failed") == 2  # worker thread logs the reason only
    assert "EXIT=0" in output
    REPORT["mqtt_e2e"] = {"messages": len(received), "statuses": [statuses(i) for i in range(len(requests))],
                          "transcript_in_logs": False, "python_reference_logs_transcript_on_schema_error": python_leaks,
                          "log": log.strip().splitlines()}
