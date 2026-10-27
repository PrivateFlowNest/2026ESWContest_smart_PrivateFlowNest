"""hm_context fusion vs homemind_context_fusion (PC x86_64 Docker, not a device test).

(c) Identical feature-event streams through Python FeatureStreamFusion and the C++ port must give identical
    debug records (accepted/reason/input_status/context event incl. evidence, confidence, transitions,
    timestamps), and identical rejections. Streams: context-fusion scenarios/, recorded artifacts/*.jsonl,
    and seeded random streams that hit TTL expiry, hysteresis, stale/future/low-confidence inputs,
    duplicates, out-of-order events, threshold boundaries and schema violations. The engine and classify()
    are also compared directly through ContextObservation.from_mapping (scenarios/demo.jsonl + random).
"""

from __future__ import annotations

import copy
import json
import random
from datetime import datetime, timedelta, timezone
from typing import Any

import pytest

from context_support import FUSION, POLICY, POLICY_HOLD, container, ensure_built, probe, report, strip_ids

ENGINES: dict[str, dict[str, Any] | None] = {
    "default": None,
    "policy-v6": {"policy": POLICY},
    "policy-v6.3-hold15": {"policy": POLICY_HOLD},
    "custom": {"engine": {"activation_updates": 3.0, "release_updates": 2, "change_updates": 1, "ttl_seconds": 7.5,
                          "quiet_audio_confidence": 0.6, "meeting_tv_margin": 0.1}},
}
CONTEXTS = ["QUIET", "SELF_TALK", "TV_MEDIA", "ZOOM_SPEAKER", "ZOOM_HEADPHONES", "IN_PERSON_MEETING", "UNKNOWN"]
ZONES = ["BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"]
LIVE = {"AUDIO_CONTEXT": ("pi5.audio-context", "NODE-RPI5-02"), "TURN_TAKING": ("pi5.turn-taking", "NODE-RPI5-02"),
        "PRESENCE": ("pi5.vision-presence", "NODE-RPI5-02"), "DEVICE_STATE": ("pi4.device-state", "NODE-RPI4-01")}
BASE = datetime(2026, 9, 3, 10, 0, tzinfo=timezone.utc)


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    ensure_built()


# ---- Python reference ------------------------------------------------------------------------------------------
def python_engine(spec: dict[str, Any] | None):
    from homemind_context_fusion.engine import ContextFusionEngine
    from homemind_context_fusion.policy_config import ContextFusionPolicy

    if spec is None:
        return ContextFusionEngine()
    if "policy" in spec:
        return ContextFusionPolicy.from_json(spec["policy"]).engine()
    return ContextFusionPolicy.from_mapping(spec["engine"]).engine()


def error_kind(error: Exception) -> str:
    from jsonschema import ValidationError

    if isinstance(error, ValidationError):
        return "ValidationError"
    return "ValueError" if isinstance(error, ValueError) else type(error).__name__


def python_stream(events: list[dict[str, Any]], spec: dict[str, Any] | None) -> list[dict[str, Any]]:
    from homemind_context_fusion.stream_fusion import FeatureStreamFusion

    fusion = FeatureStreamFusion(engine=python_engine(spec))
    out = []
    for event in events:
        try:
            out.append(fusion.ingest(copy.deepcopy(event)).debug_record())
        except Exception as error:  # noqa: BLE001 - parity of the rejection type
            out.append({"error": error_kind(error)})
    return out


def native_spec(spec: dict[str, Any] | None) -> dict[str, Any]:
    if spec is None:
        return {}
    if "policy" in spec:
        return {"policy": container(spec["policy"])}
    return {"engine": spec["engine"]}


def native_stream(streams: list[tuple[list[dict[str, Any]], dict[str, Any] | None]]) -> list[list[dict[str, Any]]]:
    requests, sizes = [], []
    for events, spec in streams:
        requests.append({"op": "fusion_new", **native_spec(spec)})
        requests += [{"op": "fusion_ingest", "event": event} for event in events]
        sizes.append(len(events))
    answers = probe(requests)
    out, position = [], 0
    for size in sizes:
        assert answers[position] == {"ok": True}, answers[position]
        chunk = answers[position + 1:position + 1 + size]
        out.append([{"error": a["error"]["type"]} if "error" in a else a for a in chunk])
        position += 1 + size
    return out


def compare(name: str, python: list[dict[str, Any]], native: list[dict[str, Any]]) -> list[Any]:
    mismatches = []
    for index, (py, cc) in enumerate(zip(python, native)):
        if strip_ids(py) != strip_ids(cc):
            mismatches.append((name, index, py, cc))
    assert len(python) == len(native)
    return mismatches


# ---- streams ---------------------------------------------------------------------------------------------------
def iso(moment: datetime, rng: random.Random) -> str:
    style = rng.random()
    if style < 0.45:
        return moment.isoformat().replace("+00:00", "Z")
    if style < 0.8:
        return moment.isoformat()
    if style < 0.9:
        return moment.astimezone(timezone(timedelta(hours=9))).isoformat()
    return moment.isoformat(timespec="milliseconds")


def boundary(rng: random.Random, points: list[float], low: float = 0.0, high: float = 1.0) -> float:
    if rng.random() < 0.35:
        return min(high, max(low, rng.choice(points) + rng.choice([-1e-3, 0.0, 0.0, 1e-3])))
    return round(rng.uniform(low, high), rng.choice([2, 3, 6]))


def payload_for(event_type: str, rng: random.Random) -> dict[str, Any]:
    if event_type == "AUDIO_CONTEXT":
        context = rng.choice(CONTEXTS)
        payload: dict[str, Any] = {
            "context_type": context, "confidence": boundary(rng, [0.5, 0.6, 0.65]),
            "speech_ratio": 0.0 if rng.random() < 0.1 else boundary(rng, [0.08, 0.12, 0.18]),
            "duration_seconds": rng.choice([4, 4.0, 20, 19.999, round(rng.uniform(0, 90), 3)]),
            "model_version": rng.choice(["yamnet-temporal-cost-ensemble-v0.8.0-50-50", "test-audio-v1"])}
        if rng.random() < 0.6:
            labels = rng.sample(["SELF_TALK", "TV_MEDIA", "ZOOM_SPEAKER", "IN_PERSON_MEETING", "UNKNOWN"],
                                rng.randint(1, 5))
            probabilities = {label: round(rng.random(), 6) for label in labels}
            if "IN_PERSON_MEETING" in probabilities and "TV_MEDIA" in probabilities and rng.random() < 0.5:
                probabilities["TV_MEDIA"] = round(min(1.0, probabilities["IN_PERSON_MEETING"] - rng.choice(
                    [0.1, 0.05, 0.0, -0.05]) if probabilities["IN_PERSON_MEETING"] >= 0.1 else 0.0), 6)
                probabilities["TV_MEDIA"] = max(0.0, probabilities["TV_MEDIA"])
            payload["class_probabilities"] = probabilities
        return payload
    if event_type == "TURN_TAKING":
        return {"turns_per_minute": boundary(rng, [1.5, 3.0], 0.0, 9.0), "confidence": boundary(rng, [0.5]),
                "method": rng.choice(["SPEAKER_CHANGE", "SPEECH_BURST_PROXY"])}
    if event_type == "PRESENCE":
        return {"presence_zone": rng.choice(ZONES), "confidence": boundary(rng, [0.4, 0.5])}
    return {"tv_on": rng.random() < 0.3, "laptop_active": rng.random() < 0.5, "headphones_connected": rng.random() < 0.3}


def corrupt(event: dict[str, Any], rng: random.Random) -> dict[str, Any]:
    bad = copy.deepcopy(event)
    choice = rng.randrange(10)
    if choice == 0:
        del bad[rng.choice(list(bad))]
    elif choice == 1:
        bad["source_direction"] = "LEFT"
    elif choice == 2:
        bad["source_node_id"] = "NODE-RPI5-01"
    elif choice == 3:
        bad["simulated"] = 1
    elif choice == 4:
        bad["payload"]["unexpected"] = True
    elif choice == 5:
        bad["timestamp"] = "yesterday"
    elif choice == 6:
        bad["valid_until"] = bad["timestamp"]
    elif choice == 7:
        bad["schema_version"] = 1.0
    elif choice == 8 and bad["event_type"] == "AUDIO_CONTEXT":
        bad["payload"]["class_probabilities"] = {"QUIET": 0.5}
    else:
        bad["event_type"] = "SOURCE_DIRECTION"
    return bad


def random_stream(seed: int, count: int = 400) -> list[dict[str, Any]]:
    rng = random.Random(seed)
    now = BASE + timedelta(seconds=rng.randint(0, 3600))
    events: list[dict[str, Any]] = []
    for index in range(count):
        roll = rng.random()
        if events and roll < 0.03:
            events.append(copy.deepcopy(rng.choice(events)))  # duplicate id
            continue
        step = rng.choice([0.5, 1.0, 1.0, 2.0]) if rng.random() > 0.06 else rng.uniform(8, 30)
        now += timedelta(seconds=step, microseconds=rng.choice([0, 0, rng.randrange(1_000_000)]))
        moment = now - timedelta(seconds=rng.uniform(0.5, 6)) if roll < 0.06 else now  # late/out of order
        event_type = rng.choices(list(LIVE), weights=[45, 15, 15, 15])[0]
        simulated_scenario = rng.random() < 0.6
        source, node = ("pc.scenario", "PC-SIMULATION") if simulated_scenario else LIVE[event_type]
        ttl = rng.choice([5, 10, 12, 20, 1.5])
        if event_type != "AUDIO_CONTEXT" and rng.random() < 0.15:
            moment = now + timedelta(seconds=rng.uniform(0.1, 2))  # future auxiliary value
        event = {"schema_version": "1.0", "event_id": f"s{seed}-{index}", "trace_id": f"stream-{seed}",
                 "timestamp": iso(moment, rng), "valid_until": iso(moment + timedelta(seconds=ttl), rng),
                 "source": source, "source_node_id": node, "event_type": event_type,
                 "simulated": True if simulated_scenario else rng.random() < 0.3,
                 "payload": payload_for(event_type, rng)}
        events.append(corrupt(event, rng) if rng.random() < 0.05 else event)
    return events


def scenario_events() -> list[dict[str, Any]]:
    out = []
    for path in sorted((FUSION / "scenarios").glob("*.jsonl")) + sorted((FUSION / "artifacts").glob("*.jsonl")):
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                value = json.loads(line)
                if isinstance(value, dict) and value.get("event_type") in (
                        "AUDIO_CONTEXT", "TURN_TAKING", "PRESENCE", "DEVICE_STATE", "SOURCE_DIRECTION"):
                    out.append(value)
    return out


def test_stream_fusion_parity() -> None:
    streams: list[tuple[str, list[dict[str, Any]], dict[str, Any] | None]] = []
    async_features = [json.loads(line) for line in
                      (FUSION / "scenarios/async_features.jsonl").read_text(encoding="utf-8").splitlines() if line]
    for name, spec in ENGINES.items():
        streams.append((f"async_features/{name}", async_features, spec))
        streams.append((f"recorded/{name}", scenario_events(), spec))
        for seed in range(4):
            streams.append((f"random{seed}/{name}", random_stream(seed + 100 * len(name)), spec))
    natives = native_stream([(events, spec) for _, events, spec in streams])
    mismatches, totals = [], {"events": 0, "fused": 0, "errors": 0, "transitions": {}}
    for (name, events, spec), native in zip(streams, natives):
        python = python_stream(events, spec)
        mismatches += compare(name, python, native)
        totals["events"] += len(events)
        for record in python:
            if "error" in record:
                totals["errors"] += 1
            elif record.get("context_event"):
                totals["fused"] += 1
                transition = record["context_event"]["transition"]
                totals["transitions"][transition] = totals["transitions"].get(transition, 0) + 1
    totals["streams"] = len(streams)
    totals["mismatches"] = len(mismatches)
    report("c_fusion_stream", totals)
    assert not mismatches, json.dumps(mismatches[:3], indent=1, ensure_ascii=False, default=str)
    assert totals["transitions"].get("ACTIVATED", 0) > 5 and totals["transitions"].get("DEACTIVATED", 0) > 5
    assert totals["errors"] > 20


# ---- engine / classify through ContextObservation.from_mapping ---------------------------------------------------
def random_observation(rng: random.Random, moment: datetime, index: int) -> dict[str, Any]:
    flat = {"timestamp": iso(moment, rng), "trace_id": f"obs-{index}",
            "audio_context": rng.choice(CONTEXTS), "audio_confidence": boundary(rng, [0.5, 0.6]),
            "speech_ratio": boundary(rng, [0.08, 0.12, 0.18]),
            "duration_seconds": rng.choice([0, 20, 19.5, round(rng.uniform(0, 60), 2)]),
            "presence_zone": rng.choice(ZONES), "presence_confidence": boundary(rng, [0.4, 0.5])}
    if rng.random() < 0.7:
        flat["turns_per_minute"] = boundary(rng, [1.5, 3.0], 0.0, 8.0)
        flat["turn_taking_method"] = rng.choice(["SPEAKER_CHANGE", "SPEECH_BURST_PROXY", "UNKNOWN"])
    for key in ("tv_on", "laptop_active", "headphones_connected"):
        if rng.random() < 0.7:
            flat[key] = rng.random() < 0.4
    if rng.random() < 0.5:
        flat["audio_class_probabilities"] = {"IN_PERSON_MEETING": round(rng.random(), 4),
                                             "TV_MEDIA": round(rng.random(), 4)}
    if rng.random() < 0.5:
        flat["audio_model_version"] = "yamnet-v"
    if rng.random() < 0.4:  # nested service form
        nested = {"timestamp": flat.pop("timestamp"), "trace_id": flat.pop("trace_id"),
                  "audio": {"context_type": flat.pop("audio_context"), "confidence": flat.pop("audio_confidence"),
                            "class_probabilities": flat.pop("audio_class_probabilities", None),
                            "model_version": flat.pop("audio_model_version", None)},
                  "vision": {"zone": flat.pop("presence_zone"), "confidence": flat.pop("presence_confidence")},
                  "features": {"speech_ratio": flat.pop("speech_ratio"), "duration_seconds": flat.pop("duration_seconds")},
                  "device_state": {k: flat.pop(k) for k in ("tv_on", "laptop_active", "headphones_connected") if k in flat}}
        if "turns_per_minute" in flat:
            nested["features"]["turns_per_minute"] = flat.pop("turns_per_minute")
            nested["features"]["turn_taking_method"] = flat.pop("turn_taking_method")
        flat = nested
    if rng.random() < 0.03:
        flat["transcript"] = "forbidden"
    return flat


def test_engine_and_classify_parity() -> None:
    from homemind_context_fusion.models import ContextObservation
    from homemind_context_fusion.scoring import classify

    demo = [json.loads(line) for line in (FUSION / "scenarios/demo.jsonl").read_text(encoding="utf-8").splitlines() if line]
    rng = random.Random(5)
    moment = BASE
    observations = list(demo)
    for index in range(600):
        moment += timedelta(seconds=rng.choice([1, 2, 2, 3]) if rng.random() > 0.05 else rng.uniform(8, 20))
        observations.append(random_observation(rng, moment, index))

    mismatches, compared = [], 0
    for name, spec in ENGINES.items():
        engine = python_engine(spec)
        quiet = getattr(engine, "quiet_audio_confidence", 0.0)
        margin = getattr(engine, "meeting_tv_margin", 0.0)
        requests = [{"op": "engine_new", **native_spec(spec)}]
        python = []
        for observation in observations:
            requests.append({"op": "engine_update", "observation": observation})
            requests.append({"op": "classify", "observation": observation, "quiet_audio_confidence": quiet,
                             "meeting_tv_margin": margin})
            try:
                parsed = ContextObservation.from_mapping(observation)
            except Exception as error:  # noqa: BLE001
                python += [{"error": error_kind(error)}] * 2
                continue
            try:
                python.append(engine.update(parsed))
            except Exception as error:  # noqa: BLE001
                python.append({"error": error_kind(error)})
            c = classify(parsed, quiet_audio_confidence=quiet, meeting_tv_margin=margin)
            python.append({"context_type": c.context_type, "confidence": c.confidence, "evidence": list(c.evidence),
                           "scores": c.scores})
        answers = probe(requests)
        assert answers[0] == {"ok": True}
        native = [{"error": a["error"]["type"]} if "error" in a else a for a in answers[1:]]
        mismatches += compare(f"engine/{name}", python, native)
        compared += len(python)
    report("c_fusion_engine", {"observations": len(observations), "compared_records": compared,
                               "mismatches": len(mismatches)})
    assert not mismatches, json.dumps(mismatches[:3], indent=1, ensure_ascii=False, default=str)


def test_formatting_helpers_match_python() -> None:
    rng = random.Random(9)
    values = [rng.uniform(-2, 2) for _ in range(3000)] + [0.125, 0.375, 2.675, 1e-05, 0.5, 1.5, 2.5, -0.0, 1e16, 1e-7]
    values += [round(rng.random(), rng.randint(1, 8)) + rng.choice([0, 5e-7, 5e-5, 5e-3]) for _ in range(2000)]
    rounds = [[v, rng.choice([0, 1, 2, 3, 4, 6])] for v in values]
    texts = ["2026-09-03T10:00:00Z", "2026-09-03T10:00:00.5+09:00", "2026-09-03 10:00", "2026-09-03",
             "2026-09-03T10:00:00.1234567Z", "20260903T100000", "2026-13-01T00:00:00Z", "bad", "2026-09-03T25:00:00"]
    stamps = [rng.randrange(1_700_000_000_000_000, 1_900_000_000_000_000) for _ in range(200)] + [
        1_788_429_600_000_000, 1_788_429_600_000_001]
    [answer] = probe([{"op": "format", "round": rounds, "fixed": rounds, "repr": values, "parse_iso": texts,
                       "iso": stamps}])
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)

    def parse(text: str) -> int | None:
        try:
            value = datetime.fromisoformat(text.replace("Z", "+00:00"))
        except ValueError:
            return None
        if value.tzinfo is None:
            value = value.replace(tzinfo=timezone.utc)
        delta = value.astimezone(timezone.utc) - epoch
        return (delta.days * 86400 + delta.seconds) * 1_000_000 + delta.microseconds

    expected_round = [round(v, n) for v, n in rounds]
    expected_fixed = [f"{v:.{n}f}" for v, n in rounds]
    expected_repr = [repr(v) for v in values]
    expected_iso = [(epoch + timedelta(microseconds=s)).isoformat() for s in stamps]
    expected_ms = [(epoch + timedelta(microseconds=s)).isoformat(timespec="milliseconds").replace("+00:00", "Z")
                   for s in stamps]
    assert answer["round"] == expected_round
    assert answer["fixed"] == expected_fixed
    assert answer["repr"] == expected_repr
    assert answer["parse_iso"] == [parse(t) for t in texts]
    assert answer["iso"] == expected_iso and answer["iso_ms"] == expected_ms
