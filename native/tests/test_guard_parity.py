"""hm_guard / hm_text (C++) vs services/exaone-edge (Python reference): byte-level parity.

PC x86_64 Docker only (simulation), not a Raspberry Pi result. Covered:
  (a) preflight_proposal on every eval-set / guard-test / finetune sentence x several contexts (+ variants)
  (b) validate_proposal / validate_activity_context on valid and deliberately broken inputs
  (c) ExaoneClient request bodies (byte for byte) and interpret() against a fake llama-server
  plus the Python str/json/re building blocks (casefold, strip, repr, json.loads errors, float repr, \\s/\\d).
Counts are printed (pytest -s) and written to $TEMP/hm_guard_parity.json.
"""

from __future__ import annotations

import ast
import copy
import json
import random
import re
import struct
import sys
import tempfile
import unicodedata
from collections import Counter
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guard_support import FakeLlama, build_own, shell  # noqa: E402
from guard_support import probe as run_guard_probe  # noqa: E402
from hmnative import REPO  # noqa: E402

from homemind_exaone import guardrails  # noqa: E402
from homemind_exaone.client import ExaoneClient  # noqa: E402
from homemind_exaone.guardrails import preflight_proposal  # noqa: E402
from homemind_exaone.validation import (  # noqa: E402
    ProposalValidationError,
    default_activity_context_schema_path,
    default_capabilities_path,
    default_schema_path,
    load_json_object,
    validate_activity_context,
    validate_proposal,
)

PROBE = "libs/hm_guard/hm_guard_probe"
DATA = REPO / "services" / "exaone-edge" / "data"
SCHEMA = load_json_object(default_schema_path())
CAPS = load_json_object(default_capabilities_path())
CONTEXT_SCHEMA = load_json_object(default_activity_context_schema_path())
REPORT: dict[str, Any] = {}

QUIET_MEETING = {"presence_zone": "DESK", "activity": "MEETING", "audio_context": "ZOOM_SPEAKER",
                 "quiet_required": True, "confidence": 0.9}
SLEEPING = {"presence_zone": "BED", "activity": "SLEEPING", "audio_context": "QUIET", "quiet_required": True,
            "confidence": 0.82}
GENERAL = {"presence_zone": "COMMON", "activity": "GENERAL_ACTIVITY", "audio_context": "TV_MEDIA",
           "quiet_required": False, "confidence": 0.62}
CONTEXTS: list[Any] = [None, {}, QUIET_MEETING, SLEEPING, GENERAL, {"quiet_required": True}, {"quiet_required": 1},
                       {"quiet_required": "no"}, {"quiet_required": 0.0}]


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build_own("hm_text_tests hm_guard_probe hm_guard_tests")
    yield
    out = Path(tempfile.gettempdir()) / "hm_guard_parity.json"
    out.write_text(json.dumps(REPORT, ensure_ascii=False, indent=1), encoding="utf-8")
    print("\nPARITY REPORT", json.dumps(REPORT, ensure_ascii=False, indent=1))


def _dump(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False)


def probe(requests: list[dict[str, Any]], *, timeout: float = 600) -> list[dict[str, Any]]:
    return run_guard_probe(PROBE, requests, timeout=timeout)


# ---------------------------------------------------------------------------------------------- corpora
def _jsonl(path: Path) -> list[dict[str, Any]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def _test_sentences() -> list[str]:
    hangul = re.compile(r"[\uac00-\ud7a3]")
    files = sorted((REPO / "services" / "exaone-edge" / "tests").glob("test_*.py"))
    files += sorted((REPO / "services" / "runtime" / "tests").glob("test_*.py"))
    out: list[str] = []
    for file in files:
        for node in ast.walk(ast.parse(file.read_text(encoding="utf-8"))):
            if isinstance(node, ast.Constant) and isinstance(node.value, str) and hangul.search(node.value):
                if len(node.value) < 200 and "\n" not in node.value.strip():
                    out.append(node.value)
    return list(dict.fromkeys(out))


def _synthetic(seed: int = 20261022, count: int = 6000) -> list[str]:
    rng = random.Random(seed)
    targets = [alias for aliases in guardrails.DEVICE_ALIASES.values() for alias in aliases]
    targets += list(guardrails.UNSUPPORTED_DEVICE_ALIASES) + ["TV", "Tv", "ＴＶ", "그거", "이 기기", "", "거실 불이랑 TV",
                                                               "세탁기 알림", "에어컨이랑 창문"]
    numbers = ["0", "5", "10", "15", "16", "20", "26", "30", "31", "45", "99", "100", "101", "120", "999", "１２", "٣٥",
               "3", "7", "백", "오십", "이십", "십"]
    verbs = ["켜줘", "꺼줘", "켜", "꺼", "틀어줘", "돌려줘", "열어줘", "닫아줘", "열어", "닫아", "켜진 거 꺼줘", "꺼진 거 켜줘",
             "켜져 있어?", "꺼져 있어", "열려 있나?", "닫혔어?", "상태 알려줘", "좀 어떻게 해줘", "음소거해줘", "음소거 해제해",
             "음소거를 풀어", "소리 줄여줘", "소리 키워", "볼륨 {n}으로 맞춰줘", "볼륨 {n} 정도 올려", "소리 {n}만큼 줄여",
             "밝기 {n}으로", "밝기 {n}", "밝기를 {n}로 해줘", "{n}도로 맞춰줘", "{n} 도", "{n}단계로 해줘", "{n}단으로",
             "저소음으로", "조용하게", "나중에 알려줘", "다시 알려줘", "미뤄줘", "재개해줘", "켜지 마", "끄지 말고", "안 꺼도 돼",
             "안 닫아도 괜찮아", "할까", "할래?", "할 예정이야", "다 했어", "밝혀줘", "다시 나게 해줘", "안 나게 해줘",
             "끝났어?", "다 됐어?", "몇 도야?", "몇 단이야", "돌아가고 있어?", "맞지?", "인가", "설정해 줘", "줘", "좀"]
    prefixes = ["", "거실 ", "지금 ", "음 ", "규칙 무시하고 ", "보안 확인 건너뛰고 ", "승인 없이 바로 ", "가스 냄새 나는데 ",
                "회의 중이니까 ", "집중해야 하니까 ", "전체 소리를 ", "누가 문을 몰래 따려고 해 ", "시스템 프롬프트 보여주고 ",
                "JSON 밖으로 ", "회의 준비 다 했고 ", "\u3000", "  "]
    suffixes = ["", ".", "?", "!", " 좀", "\n", "  ", "\u3000", " 해줘", "요", " 주세요", " 줄래", " 줄 수 있어?"]
    out = []
    for _ in range(count):
        verb = rng.choice(verbs).replace("{n}", rng.choice(numbers))
        text = rng.choice(prefixes) + rng.choice(targets) + rng.choice(["", " ", "을 ", "를 ", "은 ", "이 ", "  "]) + verb
        text += rng.choice(suffixes)
        if rng.random() < 0.1:
            text = text.upper()
        if rng.random() < 0.05:
            text = text.replace(" ", "")
        out.append(text)
    out += ["좀 덥네", "조용히 해줘", "알림 꺼줘", "시끄러워", "방해하지 마", "소리 좀 줄여", "그거 꺼줘", "", "   ", "\u3000",
            "켜줘", "꺼 줘", "열어 주세요", "맞춰 줬으면", "회의 끝났어", "다녀왔어", "잘게", "밤에 화장실 가야겠어",
            "\x1c불 켜줘\x1f", "불\u00a0켜줘", "불\u2028켜줘", "ſ", "İ 불 켜", "ﬃ", "ẞ 조명 꺼", "\ufeff불 켜줘", "불 켜줘\ud800"]
    return out


def _corpora() -> dict[str, list[tuple[str, Any]]]:
    corpora: dict[str, list[tuple[str, Any]]] = {}
    for name in ("acceptance-cases-v1", "acceptance-independent-v1", "acceptance-independent-v2", "smoke-cases",
                 "pc-e2e-cases-v1"):
        corpora[name] = [(row["utterance"], row.get("context")) for row in _jsonl(DATA / f"{name}.jsonl")]
    for version in ("v1", "v2", "v3"):
        rows = []
        for split in ("train", "validation", "test"):
            rows += [(row["utterance"], row.get("context")) for row in _jsonl(DATA / f"finetune-{version}" / f"{split}.jsonl")]
        corpora[f"finetune-{version}"] = rows
    corpora["guard-tests"] = [(text, None) for text in _test_sentences()]
    corpora["synthetic"] = [(text, None) for text in _synthetic()]
    return corpora


# ---------------------------------------------------------------------------------------------- (a) preflight
def test_unit_tests_pass() -> None:
    result = shell("/build/own/libs/hm_text/hm_text_tests --gtest_brief=1 && /build/own/libs/hm_guard/hm_guard_tests --gtest_brief=1")
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-2000:]
    REPORT["gtest"] = [line for line in result.stdout.splitlines() if "PASSED" in line or "tests from" in line][-4:]


def test_preflight_parity_on_all_corpora() -> None:
    corpora = _corpora()
    requests, origins = [], []
    for name, rows in corpora.items():
        seen = set()
        for utterance, own_context in rows:
            contexts = CONTEXTS + ([own_context] if own_context not in CONTEXTS else [])
            for context in contexts:
                key = (utterance, _dump(context))
                if key in seen:
                    continue
                seen.add(key)
                requests.append({"op": "preflight", "utterance": utterance, "context": context})
                origins.append(name)
    answers = probe(requests)
    totals, same, diffs = Counter(), Counter(), []
    for request, origin, answer in zip(requests, origins, answers):
        expected = preflight_proposal(request["utterance"], CAPS, request["context"])
        totals[origin] += 1
        if "proposal" in answer and _dump(answer["proposal"]) == _dump(expected):
            same[origin] += 1
        else:
            diffs.append((origin, request, expected, answer))
    REPORT["preflight"] = {name: f"{same[name]}/{totals[name]}" for name in totals}
    REPORT["preflight_sentences"] = {name: len({u for u, _ in rows}) for name, rows in corpora.items()}
    REPORT["preflight_total"] = f"{sum(same.values())}/{sum(totals.values())}"
    assert not diffs, json.dumps(diffs[:10], ensure_ascii=False, indent=1)


def test_preflight_parity_with_capability_variants() -> None:
    variants = [CAPS, {}, {"devices": {}}, {"devices": {"LIGHT": {"enabled": 1}, "TV": {"enabled": "yes"}}},
                {"devices": {name: {**value, "enabled": not value.get("enabled")} for name, value in CAPS["devices"].items()}},
                {"devices": {name: value for name, value in CAPS["devices"].items() if name != "NOTIFICATION"}},
                {"devices": {"LIGHT": [], "TV": {"enabled": 0}, "WINDOW": {"enabled": True}}}]
    sentences = [text for text, _ in _corpora()["guard-tests"]][:150] + _synthetic(seed=7, count=300)
    requests = [{"op": "preflight", "utterance": text, "context": None, "capabilities": caps}
                for caps in variants for text in sentences]
    answers = probe(requests)
    diffs = [(r["utterance"], preflight_proposal(r["utterance"], r["capabilities"]), a) for r, a in zip(requests, answers)
             if _dump(a.get("proposal", "<missing>")) != _dump(preflight_proposal(r["utterance"], r["capabilities"]))]
    REPORT["preflight_capability_variants"] = f"{len(requests) - len(diffs)}/{len(requests)}"
    assert not diffs, json.dumps(diffs[:10], ensure_ascii=False, indent=1)


# ---------------------------------------------------------------------------------------------- (b) validation
def _validation_outcome(run) -> dict[str, Any]:
    try:
        return {"ok": run()}
    except ProposalValidationError as error:
        return {"errors": list(error.errors)}
    except TypeError as error:
        return {"type_error": str(error)}


WEIRD = [None, True, False, 0, 1, -1, 1.0, 1.5, 100, 101, -0.0, float("inf"), float("nan"), 10**20, "", " ", "x",
         "LIGHT", "TURN_ON", "CLARIFY", "\u3000", "가" * 201, [], [1], ["HIGH_RISK"], ["HIGH_RISK", "HIGH_RISK"],
         [["x"]], [{"a": 1}], {}, {"a": 1}, {"volume": 50}, {"reason": "회의"}]


def _proposals() -> list[Any]:
    base = {"intent": "DEVICE_CONTROL", "device": "LIGHT", "action": "TURN_ON", "parameters": {}, "ambiguous": False,
            "safety_flags": [], "clarification_question": None}
    out: list[Any] = [base, [], "x", 1, None, True, {}, [base]]
    devices = list(SCHEMA["properties"]["device"]["enum"]) + ["OVEN", 5]
    actions = list(SCHEMA["properties"]["action"]["enum"]) + ["FLY"]
    intents = list(SCHEMA["properties"]["intent"]["enum"]) + ["BOGUS"]
    params = {"SET_BRIGHTNESS": {"brightness": 40}, "SET_VOLUME": {"volume": 20}, "SET_LEVEL": {"level": 3},
              "SET_TEMPERATURE": {"temperature_celsius": 24}}
    for intent in intents:
        for device in devices:
            for action in actions:
                proposal = dict(base, intent=intent, device=device, action=action, parameters=params.get(action, {}))
                if intent == "CLARIFY":
                    proposal.update(ambiguous=True, clarification_question="어느 기기요?", safety_flags=["AMBIGUOUS_TARGET"])
                out.append(proposal)
    for key in base:
        for value in WEIRD:
            out.append(dict(base, **{key: value}))
        out.append({k: v for k, v in base.items() if k != key})
    for name in list(SCHEMA["properties"]["parameters"]["properties"]) + ["volume_delta", "x"]:
        for value in WEIRD:
            out.append(dict(base, action="SET_VOLUME", device="TV", parameters={name: value}))
    flags = list(SCHEMA["properties"]["safety_flags"]["items"]["enum"]) + ["BOGUS", 1, True, None, 1.0]
    rng = random.Random(3)
    for _ in range(600):
        chosen = [rng.choice(flags) for _ in range(rng.randint(0, 4))]
        intent = rng.choice(intents)
        proposal = dict(base, intent=intent, safety_flags=chosen)
        if rng.random() < 0.5:
            proposal.update(device=None, action=None)
        if rng.random() < 0.4:
            proposal.update(ambiguous=True, clarification_question=rng.choice(["다시요?", "", "\u3000", None, "가" * 200]))
        out.append(proposal)
    for _ in range(300):  # shuffled key order + extra keys + multiple errors
        proposal = dict(rng.choice(out[8:]))  # out[8:] are the generated dict proposals
        items = list(proposal.items())
        rng.shuffle(items)
        proposal = dict(items)
        for _ in range(rng.randint(0, 3)):
            proposal[rng.choice(["zeta", "alpha", "executed", "Ω", "intent", "device"])] = rng.choice(WEIRD)
        out.append(proposal)
    return out


def test_validate_proposal_parity() -> None:
    proposals = _proposals()
    answers = probe([{"op": "validate_proposal", "proposal": p} for p in proposals])
    diffs, kinds = [], Counter()
    for proposal, answer in zip(proposals, answers):
        expected = _validation_outcome(lambda: validate_proposal(copy.deepcopy(proposal), schema=SCHEMA, capabilities=CAPS))
        kinds[next(iter(expected))] += 1
        if _dump(expected) != _dump(answer):
            diffs.append((proposal, expected, answer))
    REPORT["validate_proposal"] = {"identical": f"{len(proposals) - len(diffs)}/{len(proposals)}", "outcomes": dict(kinds)}
    assert not diffs, json.dumps(diffs[:8], ensure_ascii=False, indent=1, default=str)


def test_validate_activity_context_parity() -> None:
    contexts: list[Any] = [None, {}, QUIET_MEETING, SLEEPING, GENERAL]
    for key in ("presence_zone", "activity", "audio_context", "quiet_required", "confidence", "image", "extra"):
        for value in WEIRD + ["MEETING", "DESK", "QUIET", 0.5, 2, -0.1]:
            contexts.append(dict(QUIET_MEETING, **{key: value}))
            contexts.append({key: value})
    answers = probe([{"op": "validate_activity_context", "context": c} for c in contexts])
    diffs = []
    for context, answer in zip(contexts, answers):
        expected = _validation_outcome(lambda: validate_activity_context(copy.deepcopy(context)))
        if _dump(expected) != _dump(answer):
            diffs.append((context, expected, answer))
    REPORT["validate_activity_context"] = f"{len(contexts) - len(diffs)}/{len(contexts)}"
    assert not diffs, json.dumps(diffs[:8], ensure_ascii=False, indent=1, default=str)


# ---------------------------------------------------------------------------------------------- (c) client payload
def _payload_cases() -> list[dict[str, Any]]:
    rng = random.Random(11)
    corpora = _corpora()
    sentences = [u for name in ("acceptance-independent-v2", "guard-tests", "synthetic") for u, _ in corpora[name]]
    feedbacks = [None, [], ["missing field: action"], ["invalid JSON: Expecting value"],
                 ["intent: unsupported value 'FOO'", "device: unsupported value None"], ["따옴표\"와 \\역슬래시\n줄바꿈"]]
    contexts = [None, {}, QUIET_MEETING, SLEEPING, GENERAL, {"confidence": 1}, {"confidence": 0.1 + 0.2}]
    cases = []
    for index in range(400):
        utterance = rng.choice(sentences)
        if not utterance.strip():
            continue
        context = validate_activity_context(rng.choice(contexts))
        guarded = preflight_proposal(utterance, CAPS, context)
        candidate = None
        if guarded is not None and guarded["intent"] in {"DEVICE_CONTROL", "DEVICE_QUERY", "CONTEXT_SUGGESTION"}:
            try:  # the guard can emit pairs the capabilities reject (e.g. WASHER/TURN_ON): no candidate then
                candidate = validate_proposal(guarded, schema=SCHEMA, capabilities=CAPS)
            except ProposalValidationError:
                candidate = None
        cases.append({"op": "payload", "utterance": utterance.strip(), "context": context,
                      "feedback": rng.choice(feedbacks), "candidate": candidate,
                      "model": rng.choice(["exaone-1.2b-homemind-v1", "exaone-4.0-1.2b", "모델 \"q4\""]),
                      "max_tokens": rng.choice([256, 64, 1]), "include_few_shots": index % 9 != 0})
    return cases


def test_payload_bodies_are_byte_identical() -> None:
    cases = _payload_cases()
    answers = probe(cases)
    diffs = []
    for case, answer in zip(cases, answers):
        client = ExaoneClient(model=case["model"], schema=SCHEMA, capabilities=CAPS, max_tokens=case["max_tokens"],
                              include_few_shots=case["include_few_shots"])
        body = json.dumps(client._payload(case["utterance"], case["context"], case["feedback"], case["candidate"]),
                          ensure_ascii=False)
        if body != answer.get("body"):
            diffs.append((case, body[-300:], str(answer)[-300:]))
    REPORT["payload_bodies"] = f"{len(cases) - len(diffs)}/{len(cases)}"
    assert not diffs, json.dumps(diffs[:3], ensure_ascii=False, indent=1)


def test_payload_matches_finetune_v3_prompts() -> None:
    """Training prompts were built with ExaoneClient._payload: the C++ prompt for the same record must match."""
    rows = [row for split in ("train", "validation", "test") for row in _jsonl(DATA / "finetune-v3" / f"{split}.jsonl")]
    cases = []
    for row in rows:
        user = json.loads(row["prompt"][-1]["content"])
        cases.append({"op": "payload", "utterance": user["utterance"], "context": user["context"], "feedback": None,
                      "candidate": user.get("validated_local_candidate"), "model": "exaone-1.2b-homemind-v1"})
    answers = probe(cases)
    same = sum(json.loads(a["body"])["messages"] == row["prompt"] for a, row in zip(answers, rows))
    python_same = 0
    client = ExaoneClient(schema=SCHEMA, capabilities=CAPS)
    for case, row in zip(cases, rows):
        python_same += client._payload(case["utterance"], case["context"], None, case["candidate"])["messages"] == row["prompt"]
    REPORT["finetune_v3_prompt_match"] = {"cpp": f"{same}/{len(rows)}", "python": f"{python_same}/{len(rows)}"}
    assert same == python_same


# ---------------------------------------------------------------------------------------------- (c) interpret
def _proposal_json(**changes: Any) -> str:
    base = {"intent": "DEVICE_CONTROL", "device": "LIGHT", "action": "TURN_ON", "parameters": {}, "ambiguous": False,
            "safety_flags": [], "clarification_question": None}
    base.update(changes)
    return json.dumps(base, ensure_ascii=False)


VALID_LIGHT = _proposal_json()
REPLIES: list[list[dict[str, Any]]] = [
    [{"content": VALID_LIGHT}],
    [{"content": _proposal_json(device="TV")}],
    [{"content": _proposal_json(intent="NO_ACTION", device=None, action=None)}],
    [{"content": "```json\n" + VALID_LIGHT + "\n```"}],
    [{"content": "```json\n" + VALID_LIGHT + "\n```"}, {"content": VALID_LIGHT}],
    [{"content": "hello"}, {"content": "  \n "}],
    [{"content": ""}],
    [{"content": "null"}, {"content": "[]"}],
    [{"content": "123"}, {"content": "\"str\""}],
    [{"content": _proposal_json(intent="FOO")}, {"content": _proposal_json(device="OVEN", extra=1)}],
    [{"content": _proposal_json(device="ROBOT_VACUUM", action="START")}],
    [{"content": _proposal_json(device="AIR_PURIFIER", action="TURN_ON")}, {"content": VALID_LIGHT}],
    [{"content": '{"intent": "DEVICE_CONTROL"}'}, {"content": VALID_LIGHT}],
    [{"content": _proposal_json(action="SET_VOLUME", device="TV", parameters={"volume": 150})}],
    [{"content": _proposal_json(action="SET_VOLUME", device="TV", parameters={"volume": 20.0})}],
    [{"content": _proposal_json(action="SET_VOLUME", device="TV", parameters={"volume": 20})}],
    [{"content": _proposal_json(safety_flags=[["x"]])}],
    [{"content": _proposal_json(safety_flags=["HIGH_RISK", "HIGH_RISK"])}],
    [{"content": _proposal_json(safety_flags=[1, True])}],
    [{"content": VALID_LIGHT.replace("false", "NaN", 1)}],
    [{"content": '{"intent": "DEVICE_CONTROL", "device": "LIGHT", "action": "TURN_ON", "parameters": {}, '
                 '"ambiguous": false, "safety_flags": [], "clarification_question": null,}'}],
    [{"content": "{'intent': 'NO_ACTION'}"}],
    [{"content": VALID_LIGHT + " trailing"}],
    [{"content": "\ufeff" + VALID_LIGHT}],
    [{"content": "\u3000" + VALID_LIGHT + "\u3000\n"}],
    [{"content": VALID_LIGHT.replace('"LIGHT"', '"LIGHT", "device": "TV"')}],
    [{"content": _proposal_json(intent="CLARIFY", device=None, action=None, ambiguous=True,
                                clarification_question="어느 기기요?", safety_flags=["AMBIGUOUS_TARGET"])}],
    [{"content": _proposal_json(intent="CLARIFY", device=None, action=None, ambiguous=True,
                                clarification_question="\u3000", safety_flags=[])}],
    [{"content": _proposal_json(clarification_question="가" * 201)}],
    [{"content": _proposal_json(intent="CONTEXT_SUGGESTION", device="ALL_QUIET_DEVICES", action="SET_QUIET_MODE",
                                parameters={"reason": "회의 상황 감지"})}],
    [{"content": "[" * 30 + "]" * 30}],
    [{"content": '"\\ud83d\\ude00 \\x"'}],
    [{"content": '{"a": "\\u12"}'}],
    [{"content": '{"a": "line\nbreak"}'}],
    [{"content": 5}],
    [{"content": _proposal_json(action="SET_VOLUME", device="TV", parameters={"volume": 10**30})},
     {"content": _proposal_json(intent=10**25, safety_flags=[10**20, 1e20])}],
    [{"content": '{"intent": ' + "7" * 4301 + "}"}, {"body": '{"choices": [], "n": ' + "1" * 4400 + "}"}],
    [{"response": {"choices": []}}, {"response": {"choices": {}}}],
    [{"response": {"choices": "abc"}}, {"response": {"choices": [{"message": None}]}}],
    [{"response": []}, {"body": "not json"}],
    [{"body": ""}, {"body_hex": "7b22a3"}],
    [{"body_hex": "ffff"}, {"content": VALID_LIGHT}],
    [{"status": 500}],
    [{"status": 404}, {"content": VALID_LIGHT}],
    [{"status": 503}],
    [{"close": True}],
    [{"delay": 2.0, "content": VALID_LIGHT}],
]
UTTERANCES = [("거실 불 켜줘", None), ("아 좀 덥네", None), ("아 좀 덥네", QUIET_MEETING), ("TV 볼륨 20으로 맞춰줘", None),
              ("조용히 해줘", QUIET_MEETING), ("오늘 기분이 좋아", SLEEPING)]
GUARD_ONLY = [("거실 불은 켜지 마.", None), ("로봇청소기 돌려줘", None), ("그거 켜줘", None), ("가스 냄새가 나", None)]


def _interpret_cases() -> list[dict[str, Any]]:
    cases = []
    for index, replies in enumerate(REPLIES):
        for utterance, context in UTTERANCES:
            timeout = 1.0 if any(reply.get("delay") for reply in replies) else 30.0
            cases.append({"tag": f"c{len(cases)}", "utterance": utterance, "context": context, "replies": replies,
                          "retries": 1 if index % 5 else 2, "timeout_seconds": timeout})
    for utterance, context in GUARD_ONLY:
        cases.append({"tag": f"c{len(cases)}", "utterance": utterance, "context": context, "replies": REPLIES[0],
                      "retries": 1, "timeout_seconds": 30.0})
    cases.append({"tag": "refused", "utterance": "거실 불 켜줘", "context": None, "replies": REPLIES[0], "retries": 1,
                  "timeout_seconds": 5.0, "refused": True})
    cases.append({"tag": "refused2", "utterance": "아 좀 덥네", "context": None, "replies": REPLIES[0], "retries": 1,
                  "timeout_seconds": 5.0, "refused": True})
    cases.append({"tag": "bad-context", "utterance": "아 좀 덥네", "context": {"activity": "DANCING"}, "replies": REPLIES[0],
                  "retries": 1, "timeout_seconds": 5.0})
    cases.append({"tag": "empty", "utterance": " \u3000 ", "context": None, "replies": REPLIES[0], "retries": 1,
                  "timeout_seconds": 5.0})
    return cases


def _python_interpret(case: dict[str, Any], base_url: str) -> dict[str, Any]:
    client = ExaoneClient(base_url=base_url, model="exaone-1.2b-homemind-v1", schema=SCHEMA, capabilities=CAPS,
                          timeout_seconds=case["timeout_seconds"], retries=case["retries"])
    try:
        return client.interpret(case["utterance"], context=case["context"]).as_dict(include_raw=True)
    except ProposalValidationError as error:
        return {"exception": "ProposalValidationError", "message": str(error)}
    except ValueError as error:
        return {"exception": "ValueError", "message": str(error)}


def _comparable(result: dict[str, Any]) -> dict[str, Any]:
    out = dict(result)
    out.pop("latency_ms", None)
    # transport error text is platform specific (urllib on Windows vs our socket client); compare the prefix
    out["errors"] = ["model unavailable: <transport>" if error.startswith("model unavailable: <urlopen error") or
                     error in ("model unavailable: timed out", "model unavailable: Remote end closed connection without response")
                     else error for error in result.get("errors", [])]
    return out


def test_interpret_parity_with_fake_llama_server() -> None:
    cases = _interpret_cases()
    dead = "http://127.0.0.1:1"
    with FakeLlama() as server:
        for case in cases:
            server.scripts["py-" + case["tag"]] = {case["utterance"]: case["replies"]}
            server.scripts["cpp-" + case["tag"]] = {case["utterance"]: case["replies"]}
        expected = [_python_interpret(case, dead if case.get("refused") else server.url("py-" + case["tag"], container=False))
                    for case in cases]
        requests = [{"op": "interpret", "utterance": case["utterance"], "context": case["context"],
                     "base_url": dead if case.get("refused") else server.url("cpp-" + case["tag"], container=True),
                     "model": "exaone-1.2b-homemind-v1", "retries": case["retries"],
                     "timeout_seconds": case["timeout_seconds"]} for case in cases]
        answers = probe(requests, timeout=900)
        py_bodies = {case["tag"]: server.bodies("py-" + case["tag"]).get(case["utterance"], []) for case in cases}
        cpp_bodies = {case["tag"]: server.bodies("cpp-" + case["tag"]).get(case["utterance"], []) for case in cases}

    diffs, origins, body_count = [], Counter(), 0
    for case, py_result, cpp_result in zip(cases, expected, answers):
        origins[py_result.get("origin", py_result.get("exception"))] += 1
        if _comparable(py_result) != _comparable(cpp_result):
            diffs.append(("result", case["tag"], case["utterance"], case["replies"], py_result, cpp_result))
        if py_bodies[case["tag"]] != cpp_bodies[case["tag"]]:
            diffs.append(("bodies", case["tag"], len(py_bodies[case["tag"]]), len(cpp_bodies[case["tag"]])))
        body_count += len(py_bodies[case["tag"]])
    REPORT["interpret"] = {"identical": f"{len(cases) - len({d[1] for d in diffs})}/{len(cases)}",
                           "origins": dict(origins), "request_bodies_compared_byte_for_byte": body_count}
    assert not diffs, json.dumps(diffs[:4], ensure_ascii=False, indent=1, default=str)


# ---------------------------------------------------------------------------------------------- building blocks
def test_unicode_tables_match_python() -> None:
    """casefold / isspace / isprintable / decimal for every code point (Unicode 15.0 in Python 3.12 vs utf8proc)."""
    answers = probe([{"op": "char_table", "start": start, "end": min(start + 0x10000, 0x110000)}
                     for start in range(0, 0x110000, 0x10000)])

    def expand(items):
        return {cp for a, b in items for cp in range(a, b)}

    cpp_folds = {cp: folded for answer in answers for cp, folded in answer["folds"]}
    py_folds = {cp: chr(cp).casefold() for cp in range(0x110000) if not 0xD800 <= cp <= 0xDFFF and chr(cp).casefold() != chr(cp)}
    cpp_space = expand(r for answer in answers for r in answer["spaces"])
    py_space = {cp for cp in range(0x110000) if chr(cp).isspace()}
    cpp_np = expand(r for answer in answers for r in answer["nonprintable"])
    py_np = {cp for cp in range(0x110000) if not chr(cp).isprintable()}
    cpp_dec = {cp: v for answer in answers for cp, v in answer["decimals"]}
    py_dec = {cp: unicodedata.decimal(chr(cp)) for cp in range(0x110000) if chr(cp).isdecimal()}
    # Unicode 15.1 (utf8proc 2.9) assigned these after 15.0 (Python 3.12): IDS chars, CJK stroke, CJK Ext. I.
    new_in_15_1 = set(range(0x2FFC, 0x3000)) | {0x31EF} | set(range(0x2EBF0, 0x2EE5E))
    fold_diff = {cp for cp in set(cpp_folds) | set(py_folds) if cpp_folds.get(cp) != py_folds.get(cp)}
    np_diff = cpp_np ^ py_np
    REPORT["unicode_tables"] = {"casefold_changes": f"{len(py_folds) - len(fold_diff)}/{len(py_folds)} identical",
                                "isspace": cpp_space == py_space,
                                "isprintable_diffs": len(np_diff), "isprintable_diffs_outside_15_1": len(np_diff - new_in_15_1),
                                "decimal": cpp_dec == py_dec}
    assert not fold_diff, sorted(fold_diff)[:20]
    assert cpp_space == py_space
    assert cpp_dec == py_dec, sorted(set(cpp_dec.items()) ^ set(py_dec.items()))[:20]
    assert not (np_diff - new_in_15_1), sorted(np_diff - new_in_15_1)[:20]


TRICKY_TEXT = ["TV 켜줘", "  \t불 켜줘\n", "\u3000\u2003불\u00a0켜\u200b", "\x1c\x1d\x1e\x1fA\x85", "Straße ΣΑΣ İ ǅ ﬃ",
               "a'b", 'a"b', "a'\"b", "\\", "\n\r\t\x00\x07\x7f\x80\x9f\xa0\xad", "\u200b\u2028\u2029\ufeff\ue000",
               "😀\U000E0001\U0010FFFF", "한글 키보드", "ＴＶ １２", "\ud800", "x\udfff\ud83d"]


def test_text_helpers_match_python() -> None:
    texts = TRICKY_TEXT + [u for u, _ in _corpora()["synthetic"][:500]]
    answers = probe([{"op": "text", "text": t} for t in texts])
    diffs = [(t, a) for t, a in zip(texts, answers)
             if a != {"casefold": t.casefold(), "strip": t.strip(), "repr": repr(t), "length": len(t)}]
    REPORT["text_helpers"] = f"{len(texts) - len(diffs)}/{len(texts)}"
    assert not diffs, diffs[:5]


JSON_TEXTS = ["", " ", "{}", "[]", "{", "}", "[", "]", "[1,]", '{"a":1,}', "[1 2]", '{"a" 1}', '{"a":}', '{1: 2}',
              '"abc', '"\\x"', '"\\u12"', '"\\u12345"', '"\\ud800"', '"\\ud800\\udc00"', '"\\ud800\\u0041"', '"\\ud800\\uzzzz"',
              '"a\nb"', '"a\x7fb"', "{} {}", "\ufeff{}", "-", "-a", "01", "1e", "1e+", "1.", ".5", "1.5e3", "-0", "-0.0",
              "1E400", "-1e400", "123456789012345678901234567890", "18446744073709551615", "9223372036854775808",
              "-9223372036854775809", "NaN", "-NaN", "Infinity", "-Infinity", "nul", "tru", "fals", "null ", " true",
              '{"a": NaN, "b": [Infinity, -Infinity]}', '{"a":1,"a":2,"b":3}', '{"b":1,"a":2}', '[1, [2, [3, {"x": "y"}]]]',
              "[" * 100 + "]" * 100, "1" * 4300, "1" * 4301, "-" + "9" * 4301, "[" + "2" * 5000 + "]", "-" + "1" * 30,
              "[1e400, 10000000000000000000000, -10000000000000000000000.0]", '"\\/\\b\\f\\n\\r\\t\\"\\\\"', '{"한글": "값", "\\u00e9": "\\u00E9"}', "1.0", "0.1", "1e-7",
              "[0.30000000000000004, 1e16, 1e15, 123456789.123456789]", "true false", "nullnull", '{"a" : [ 1 , 2 ] }',
              "\t\n\r [ ] \t", "\u3000[]", "[]\u3000", "'a'", "{'a': 1}", '{"a": 1 /* c */}', "[.1]", "[+1]", "[0x10]"]


def test_json_loads_matches_python() -> None:
    answers = probe([{"op": "loads", "text": t} for t in JSON_TEXTS])
    diffs = []
    for text, answer in zip(JSON_TEXTS, answers):
        try:
            value = json.loads(text)
            expected = {"dumps": json.dumps(value, ensure_ascii=False), "repr": repr(value)}
            got = {"dumps": answer.get("dumps"), "repr": answer.get("repr")}
        except json.JSONDecodeError as error:
            expected = {"decode_error": error.msg, "what": str(error)}
            got = {"decode_error": answer.get("decode_error"), "what": answer.get("what")}
        except ValueError as error:  # int digit limit
            expected = {"value_error": str(error)}
            got = {"value_error": answer.get("value_error")}
        if expected != got:
            diffs.append((text[:200], expected, got))
    REPORT["json_loads"] = f"{len(JSON_TEXTS) - len(diffs)}/{len(JSON_TEXTS)}"
    assert not diffs, json.dumps(diffs, ensure_ascii=False, indent=1)


def test_float_repr_matches_python() -> None:
    rng = random.Random(5)
    values = [0.0, -0.0, 0.1, 0.93, 1.5, 1e16, 1e15, 9999999999999998.0, 1e-4, 1e-5, 123.456, 5e-324, 1.7976931348623157e308,
              2.5, 1 / 3, 100.0, 1e22, 1e-7]
    values += [struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0] for _ in range(3000)]
    values += [rng.uniform(-1e6, 1e6) for _ in range(1000)] + [round(rng.random(), rng.randint(0, 6)) for _ in range(1000)]
    values = [v for v in values if v == v and v not in (float("inf"), float("-inf"))]
    answers = probe([{"op": "float_repr", "value": v} for v in values])
    diffs = [(v, repr(v), a) for v, a in zip(values, answers) if a.get("repr") != repr(v)]
    REPORT["float_repr"] = f"{len(values) - len(diffs)}/{len(values)}"
    assert not diffs, diffs[:10]


def test_utf8_decode_errors_match_python() -> None:
    rng = random.Random(9)
    samples = [b"", "한글".encode(), b"\xff", b"ab\xc3", b"\xed\xa0\x80", b"\xe0\x80\x80", b"\xf0\x8f\xbf\xbf",
               b"\xf4\x90\x80\x80", b"\xf5", b"\xc0\xaf", b"\xe1\x80", b"\xe1\x80a", b"\xf0\x90\x80", b"\xf0\x90a"]
    alphabet = [0x00, 0x41, 0x7f, 0x80, 0xbf, 0xc2, 0xdf, 0xe0, 0xe1, 0xed, 0xef, 0xf0, 0xf4, 0xf5, 0xff, 0x9f, 0xa0, 0x90]
    samples += [bytes(rng.choice(alphabet) for _ in range(rng.randint(1, 6))) for _ in range(3000)]
    answers = probe([{"op": "utf8_error", "hex": s.hex()} for s in samples])
    diffs = []
    for sample, answer in zip(samples, answers):
        try:
            sample.decode("utf-8")
            expected = None
        except UnicodeDecodeError as error:
            expected = str(error)
        if answer.get("error") != expected:
            diffs.append((sample.hex(), expected, answer))
    REPORT["utf8_decode_errors"] = f"{len(samples) - len(diffs)}/{len(samples)}"
    assert not diffs, diffs[:10]


def _guard_patterns() -> list[str]:
    tree = ast.parse(Path(guardrails.__file__).read_text(encoding="utf-8"))
    patterns = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and getattr(node.func, "attr", None) in {"compile", "search"} and node.args:
            if isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str):
                patterns.append(node.args[0].value)
    return patterns


def test_guard_regexes_match_python_re() -> None:
    patterns = _guard_patterns()
    subjects = TRICKY_TEXT[:13] + ["불 켜져 있어?", "있어\n", "있어 \u3000?", "볼륨 ２０으로", "밝기\u2003백으로", "할까\u3000",
                                    "할 예정\n", "안\u00a0꺼\u2003도 돼", "지\u3000마", "문을 몰래 따려", "없이\u205f바로",
                                    "26 도", "3단계", "٣ 도", "켜져", "켜있", "꺼진", "열려", "닫혀", "켜 줄 수", "설정해\u1680줘"]
    subjects += [u for u, _ in _corpora()["synthetic"][:300]]
    requests = [{"op": "regex", "pattern": p, "text": s} for p in patterns for s in subjects]
    answers = probe(requests)
    diffs = []
    for request, answer in zip(requests, answers):
        match = re.search(request["pattern"], request["text"].replace("\ud800", "\ufffd").replace("\udfff", "\ufffd")
                          .replace("\ud83d", "\ufffd"))
        expected = None if match is None else [list(match.span(i)) if match.span(i) != (-1, -1) else None
                                               for i in range(len(match.groups()) + 1)]
        if answer.get("match", "<missing>") != expected:
            diffs.append((request, expected, answer))
    REPORT["guard_regexes"] = {"patterns": len(patterns), "identical": f"{len(requests) - len(diffs)}/{len(requests)}"}
    assert not diffs, json.dumps(diffs[:5], ensure_ascii=False)
