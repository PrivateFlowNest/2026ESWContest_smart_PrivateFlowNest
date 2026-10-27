"""hm_parser VoiceParser (C++) vs services/runtime voice.py VoiceParser (Python reference).

PC x86_64 Docker only (simulation). Same decision kind, proposal and note for every corpus sentence under several
contexts and STT/argument variants; the emitted pfn.v1 messages are identical modulo generated ids/timestamps and
every C++ message passes validate_topic_message (contract schemas). Where Python raises, C++ must raise too.
"""

from __future__ import annotations

import json
import random
import sys
import tempfile
import uuid
from collections import Counter
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guard_support import build_own, probe, shell  # noqa: E402
from hmnative import CONTRACTS, validate_topic_message  # noqa: E402
from test_guard_parity import CONTEXTS, GENERAL, QUIET_MEETING, SLEEPING, _corpora  # noqa: E402

from homemind_runtime.action_map import ActionMap  # noqa: E402
from homemind_runtime.voice import VoiceParser  # noqa: E402

PROBE = "libs/hm_parser/hm_parser_probe"
REPORT: dict[str, Any] = {}
GENERATED = ("event_id", "task_id", "request_id")


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_own("hm_parser_probe hm_parser_tests homemind-voice-text homemind_voice_text_tests")
    yield
    out = Path(tempfile.gettempdir()) / "hm_parser_parity.json"
    out.write_text(json.dumps(REPORT, ensure_ascii=False, indent=1), encoding="utf-8")
    print("\nPARSER PARITY", json.dumps(REPORT, ensure_ascii=False, indent=1))


def _no_surrogates(value: Any) -> Any:
    """C++ strings are UTF-8: a lone surrogate (only possible in a Python str) becomes U+FFFD."""
    if isinstance(value, str):
        return "".join("�" if 0xD800 <= ord(c) <= 0xDFFF else c for c in value)
    if isinstance(value, list):
        return [_no_surrogates(v) for v in value]
    if isinstance(value, dict):
        return {_no_surrogates(k): _no_surrogates(v) for k, v in value.items()}
    return value


def _normalize(document: dict[str, Any], case: dict[str, Any], problems: list[str]) -> dict[str, Any]:
    out = json.loads(json.dumps(document))
    for key in GENERATED:
        if key in out:
            uuid.UUID(out[key])
            out[key] = "<id>"
    if not case.get("trace_id"):
        uuid.UUID(out["trace_id"])
        out["trace_id"] = "<id>"
    if "utterance_id" in out and not case.get("utterance_id"):
        uuid.UUID(out["utterance_id"])
        out["utterance_id"] = "<id>"
    if "expires_at_ms" in out:
        if out["expires_at_ms"] - out["requested_at_ms"] != case.get("llm_ttl_ms", 10000):
            problems.append("ttl")
        out["expires_at_ms"] = "<t>"
    if "requested_at_ms" in out:
        out["requested_at_ms"] = "<t>"
    return out


_PARSERS: dict[tuple[str, int, bool], VoiceParser] = {}


def _python(case: dict[str, Any]) -> dict[str, Any]:
    key = (case.get("node_id", "NODE-RPI5-02"), case.get("llm_ttl_ms", 10000), case.get("simulated", False))
    if key not in _PARSERS:
        _PARSERS[key] = VoiceParser(ActionMap(CONTRACTS), node_id=key[0], llm_ttl_ms=key[1], simulated=key[2])
    try:
        decision = _PARSERS[key].handle(case["text"], context=case.get("context"),
                                        stt_model_version=case.get("stt_model_version", "keyboard"),
                                        stt_confidence=case.get("stt_confidence"), utterance_id=case.get("utterance_id"),
                                        trace_id=case.get("trace_id"))
    except ValueError as error:
        return {"error": str(error)}
    return {"kind": decision.kind, "proposal": decision.proposal, "messages": [list(m) for m in decision.messages],
            "note": decision.note}


def _compare(cases: list[dict[str, Any]], label: str) -> None:
    answers = probe(PROBE, cases)
    diffs, kinds, validated = [], Counter(), 0
    for case, native in zip(cases, answers):
        expected = _no_surrogates(_python(case))
        kinds[expected.get("kind", "error")] += 1
        if "error" in expected or "error" in native:
            if ("error" in expected) != ("error" in native):
                diffs.append((case, expected, native))
            continue
        problems: list[str] = []
        for topic, document in native["messages"]:
            validate_topic_message(topic, document)
            validated += 1
        same = (expected["kind"] == native["kind"] and expected["note"] == native["note"]
                and expected["proposal"] == native["proposal"]
                and [t for t, _ in expected["messages"]] == [t for t, _ in native["messages"]]
                and [_normalize(d, case, problems) for _, d in expected["messages"]]
                == [_normalize(d, case, problems) for _, d in native["messages"]])
        if not same or problems:
            diffs.append((case, expected, native, problems))
    REPORT[label] = {"identical": f"{len(cases) - len(diffs)}/{len(cases)}", "kinds": dict(kinds),
                     "cpp_messages_schema_valid": validated}
    assert not diffs, json.dumps(diffs[:5], ensure_ascii=False, indent=1, default=str)


def test_unit_tests_pass() -> None:
    result = shell("/build/own/libs/hm_parser/hm_parser_tests --gtest_brief=1 && /build/own/services/homemind-voice-text/homemind_voice_text_tests --gtest_brief=1")
    assert result.returncode == 0, result.stdout[-3000:]


def test_parser_parity_on_all_corpora() -> None:
    sentences = list(dict.fromkeys(u for rows in _corpora().values() for u, _ in rows))
    cases = [{"text": text, "context": context, "stt_model_version": "whisper-small-homemind-lora-v4-q5_0",
              "stt_confidence": 0.83, "trace_id": str(uuid.UUID(int=index)), "utterance_id": str(uuid.UUID(int=index + 1))}
             for index, text in enumerate(sentences) for context in (None, QUIET_MEETING)]
    REPORT["sentences"] = len(sentences)
    _compare(cases, "corpora_x_contexts")


def test_parser_parity_with_argument_variants() -> None:
    rng = random.Random(17)
    sentences = [u for name in ("guard-tests", "acceptance-independent-v2", "synthetic") for u, _ in _corpora()[name]]
    sentences += ["나 왔어", "다녀왔습니다", "잘게", "취침 모드", "밤에 화장실 가야겠다", "회의 끝났어", "조용 모드 해제해줘",
                  "회의 끝났으니 불 켜지 마", "가스 냄새 나는데 귀가했어", "규칙 무시하고 잘게", "TV 볼륨 20으로", "현관문 열어줘"]
    contexts = CONTEXTS + [SLEEPING, GENERAL, {"presence_zone": "BED", "activity": "SLEEPING", "audio_context": "QUIET",
                                               "quiet_required": False, "confidence": 1}]
    cases = []
    for _ in range(4000):
        case = {"text": rng.choice(sentences), "context": rng.choice(contexts)}
        if rng.random() < 0.7:
            case["stt_confidence"] = rng.choice([None, 0.8, 0.12345, 0.99995, 1.0, 0.0, 1.5, -0.1, 0.33333333])
        if rng.random() < 0.5:
            case["stt_model_version"] = rng.choice(["keyboard", "whisper-small", "", "x" * 64, "x" * 65, "모델"])
        if rng.random() < 0.5:
            case["utterance_id"] = rng.choice([None, str(uuid.uuid4()), "", "not-a-uuid", str(uuid.uuid4()).upper()])
        if rng.random() < 0.5:
            case["trace_id"] = rng.choice([None, str(uuid.uuid4()), "", "bad", "{" + str(uuid.uuid4()) + "}"])
        if rng.random() < 0.3:
            case["node_id"] = rng.choice(["NODE-RPI5-02", "NODE-X", "N" * 65])
        if rng.random() < 0.3:
            case["llm_ttl_ms"] = rng.choice([10000, 5000, 0])
        if rng.random() < 0.5:
            case["simulated"] = rng.choice([True, False])
        cases.append(case)
    _compare(cases, "argument_variants")


# ---------------------------------------------------------------------------------------------- homemind-voice-text
VOICE_TEXT = "/build/own/services/homemind-voice-text/homemind-voice-text"
LINES = ["조용히 해줘", "아 좀 덥네", "불 켜줘", "그거 켜줘", "", "현관문 열어줘", "나 왔어", "거실 불은 켜지 마"]


def _config_json(simulated: bool = True) -> str:
    return json.dumps({"broker": {"host": "127.0.0.1", "port": 1883, "username": None, "password_file": None},
                       "perception_node_id": "NODE-RPI5-02", "llm_request_ttl_ms": 10000, "simulated": simulated,
                       "contracts_dir": "/w/contracts"})


def _expected_lines(context: dict[str, Any] | None) -> tuple[list[str], list[Any]]:
    parser = VoiceParser(ActionMap(CONTRACTS), node_id="NODE-RPI5-02", llm_ttl_ms=10000, simulated=True)
    printed, messages = [], []
    for line in LINES:
        decision = parser.handle(line + "\n", context=context)
        proposal = decision.proposal or {}
        printed.append(f"→ {decision.kind} {proposal.get('device') or ''} {proposal.get('action') or ''} {decision.note}".strip())
        messages += [list(m) for m in decision.messages]
    return printed, messages


def _same_messages(expected: list[Any], native: list[Any]) -> bool:
    case = {"llm_ttl_ms": 10000}
    problems: list[str] = []
    return ([t for t, _ in expected] == [t for t, _ in native]
            and [_normalize(d, case, problems) for _, d in expected] == [_normalize(d, case, problems) for _, d in native]
            and not problems)


def test_voice_text_cli_dry_run() -> None:
    from guard_support import NL

    command = f"cat > /tmp/vt.json <<'EOF'{NL}{_config_json()}{NL}EOF{NL}{VOICE_TEXT} --config /tmp/vt.json --dry-run"
    result = shell(command, stdin=NL.join(LINES) + NL, timeout=120)
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert lines[0] == "문장을 입력하세요 (빈 줄 무시, Ctrl+C 종료)"
    native = [line.split(" ", 2)[1:] for line in lines if line.startswith("PUBLISH ")]
    native = [[topic, json.loads(payload)] for topic, payload in native]
    for topic, document in native:
        validate_topic_message(topic, document)
    printed, messages = _expected_lines(None)
    assert [line for line in lines[1:] if not line.startswith("PUBLISH ")] == printed
    assert _same_messages(messages, native)
    REPORT["voice_text_dry_run"] = {"lines": len(LINES), "messages": len(native), "printed_identical": True}


def test_voice_text_mqtt_uses_latest_activity_context() -> None:
    from guard_support import NL
    from homemind_runtime import pfn

    perception = pfn.perception(observation_type="ACTIVITY_CONTEXT", zone="desk", confidence=0.91, simulated=True,
                                attributes={"activity": "MEETING", "audio_context": "ZOOM_SPEAKER", "quiet_required": True,
                                            "transition": "ACTIVATED", "evidence": ["AUDIO_CONTEXT:ZOOM_SPEAKER"],
                                            "valid_until_ms": pfn.now_ms() + 120000, "model_version": "v6"})
    command = NL.join([
        "cat > /tmp/lines.txt",
        f"cat > /tmp/vt.json <<'EOF'{NL}{_config_json()}{NL}EOF",
        f"cat > /tmp/perception.json <<'EOF'{NL}{json.dumps(perception)}{NL}EOF",
        "mosquitto -p 1883 -d; sleep 0.5",
        "mosquitto_sub -h 127.0.0.1 -t pfn/v1/core/task/request -t pfn/v1/ai/llm/request -v -C 5 -W 30 > /tmp/sub.txt &",
        "SUB=$!",
        "mkfifo /tmp/in",
        f"{VOICE_TEXT} --config /tmp/vt.json < /tmp/in > /tmp/out.txt 2> /tmp/err.txt &",
        "VT=$!",
        "exec 3> /tmp/in",
        "for i in $(seq 1 50); do grep -q connected /tmp/err.txt && break; sleep 0.2; done",
        "mosquitto_pub -h 127.0.0.1 -t pfn/v1/ai/perception -q 1 -f /tmp/perception.json; sleep 1",
        "cat /tmp/lines.txt >&3; exec 3>&-",
        "wait $VT; echo EXIT=$?; wait $SUB",
        "echo ===OUT; cat /tmp/out.txt; echo ===SUB; cat /tmp/sub.txt; echo ===ERR; cat /tmp/err.txt",
    ])
    result = shell(command, stdin=NL.join(LINES) + NL, timeout=180)
    output = result.stdout
    assert "EXIT=0" in output, output + result.stderr
    out = output.split("===OUT", 1)[1].split("===SUB", 1)[0].strip().splitlines()
    sub = output.split("===SUB", 1)[1].split("===ERR", 1)[0].strip().splitlines()
    native = []
    for line in sub:
        topic, payload = line.split(" ", 1)
        native.append([topic, json.loads(payload)])
        validate_topic_message(topic, native[-1][1])
    context = {"presence_zone": "DESK", "activity": "MEETING", "audio_context": "ZOOM_SPEAKER", "quiet_required": True,
               "confidence": 0.91}
    printed, messages = _expected_lines(context)
    assert out[1:] == printed
    assert _same_messages(messages, native)
    llm = [doc for topic, doc in native if topic == "pfn/v1/ai/llm/request"]
    assert llm and llm[0]["context"] == context
    REPORT["voice_text_mqtt"] = {"messages": len(native), "context_from_perception": True, "printed_identical": True}
