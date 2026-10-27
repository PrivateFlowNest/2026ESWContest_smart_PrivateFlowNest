"""Count quiet-mode flapping of fusion policies on long real meeting audio (PC tool).

Builds the same 30-minute stream as native/tests/soak_pc_native.py (AIHub meeting audio, 6 min meeting / 2 min quiet
room, repeated: 4 meeting episodes), extracts the AUDIO_CONTEXT feature events once with the production audio
pipeline (YAMNet + activity head), then replays them through FeatureStreamFusion for every policy that passed the
tuning gates in artifacts/audio-policy-tuning-v6-ensemble-50-50.json. Ideal for 30 min: 4 quiet activations, 3 releases
(the last meeting runs to the end).

    services/runtime/.venv/Scripts/python services/edge-ai/context-fusion/tools/flap_check.py [--minutes 30]
        [--policy services/edge-ai/context-fusion/config/audio-policy-v6.3-hold15.json] [--release-grid 8,15,20]
"""

from __future__ import annotations

import argparse
import glob
import itertools
import json
import math
import os
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parents[1]
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "services" / "runtime" / "src"))
AIHUB = Path(os.environ.get("HM_AIHUB_DIR", "E:/HomeMind-Datasets/raw/aihub"))
RATE = 16_000


def stream_audio(minutes: float) -> np.ndarray:
    import wave

    source = next(Path(p) for p in itertools.islice(glob.iglob(str(AIHUB / "464_meeting/**/*.wav"), recursive=True), 1))
    with wave.open(str(source), "rb") as reader:
        meeting = np.frombuffer(reader.readframes(reader.getnframes()), dtype="<i2")
    rng = np.random.default_rng(20261022)
    out, cursor, total, produced = [], 0, int(minutes * 60 * RATE), 0
    while produced < total:
        chunk = meeting[cursor: cursor + 6 * 60 * RATE]
        cursor = (cursor + len(chunk)) % (len(meeting) - 6 * 60 * RATE)
        quiet = rng.normal(0, 30, 2 * 60 * RATE).astype("<i2")
        out += [chunk, quiet]
        produced += len(chunk) + len(quiet)
    return (np.concatenate(out)[:total].astype(np.float32) / 32768.0)


def feature_events(audio: np.ndarray, policy_path: Path) -> list[dict]:
    from homemind_runtime.audio.context import AudioContextPipeline
    from homemind_runtime.perception import DEFAULTS

    pipeline = AudioContextPipeline(activity_model=DEFAULTS["activity_model"], yamnet_model=DEFAULTS["yamnet_model"],
                                    policy_path=policy_path, simulated=True)
    start = datetime(2026, 10, 22, tzinfo=timezone.utc)
    events = []
    window, hop = 4 * RATE, RATE
    for index, offset in enumerate(range(0, len(audio) - window + 1, hop)):
        update = pipeline.process(audio[offset: offset + window], timestamp=start + timedelta(seconds=(offset + window) / RATE),
                                  elapsed_seconds=(offset + window) / RATE)
        events.append(update.feature_event)
        if index % 300 == 0:
            print(f"  window {index}", flush=True)
    return events


def replay(events: list[dict], policy: dict) -> dict:
    from homemind_context_fusion.policy_config import ContextFusionPolicy
    from homemind_context_fusion.stream_fusion import FeatureStreamFusion

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "policy.json"
        path.write_text(json.dumps({"schema_version": "1.0", "config_version": "candidate", "engine": policy}), "utf-8")
        fusion = FeatureStreamFusion(engine=ContextFusionPolicy.from_json(path).engine())
    activations = releases = quiet_seconds = 0
    quiet = False
    for event in events:
        result = fusion.ingest(event)
        context = result.context_event
        if context is None:
            continue
        if context["transition"] == "ACTIVATED" and context["quiet_required"]:
            activations += 1
        if context["transition"] == "DEACTIVATED":
            releases += 1
        quiet = bool(context["quiet_required"]) and context["transition"] != "DEACTIVATED"
        quiet_seconds += 1 if quiet else 0
    return {"activations": activations, "releases": releases, "quiet_minutes": round(quiet_seconds / 60, 1)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=float, default=30.0)
    parser.add_argument("--out", type=Path, default=HERE / "artifacts" / "policy-flap-check-30min.json")
    parser.add_argument("--release-grid", default="", help="extra release_updates values to try on the current policy, e.g. 8,12,20,30")
    parser.add_argument("--activation-grid", default="", help="extra activation_updates values for the release grid")
    parser.add_argument("--events-cache", type=Path, help="reuse/save the extracted feature events (JSONL, no audio)")
    parser.add_argument("--policy", type=Path, action="append", default=[],
                        help="policy config file to replay as well (repeatable), e.g. config/audio-policy-v6.3-hold15.json")
    args = parser.parse_args()
    tuning = json.loads((HERE / "artifacts" / "audio-policy-tuning-v6-ensemble-50-50.json").read_text("utf-8"))
    passing = [c for c in tuning["candidates"] if c["passed"]]
    print(f"{len(passing)} passing candidates; extracting features for {args.minutes} min of audio", flush=True)
    if args.events_cache and args.events_cache.exists():
        events = [json.loads(line) for line in args.events_cache.read_text("utf-8").splitlines() if line]
    else:
        events = feature_events(stream_audio(args.minutes), HERE / "config" / "audio-policy-v6-ensemble-50-50.json")
        if args.events_cache:
            args.events_cache.write_text("\n".join(json.dumps(e, ensure_ascii=False) for e in events), "utf-8")
    rows = []
    for candidate in passing:
        stats = replay(events, candidate["policy"])
        rows.append({**{k: candidate[k] for k in ("policy", "quiet_recall", "quiet_false_positive_rate",
                                                   "exact_context_accuracy", "activation_p95_seconds",
                                                   "release_p95_seconds")}, **stats})
    base = dict(tuning["selected_candidate"]["policy"])
    for release in [int(x) for x in args.release_grid.split(",") if x]:
        for activation in [int(x) for x in (args.activation_grid or str(base["activation_updates"])).split(",") if x]:
            policy = {**base, "release_updates": release, "activation_updates": activation}
            rows.append({"policy": policy, "quiet_recall": None, "quiet_false_positive_rate": None,
                         "exact_context_accuracy": None, "activation_p95_seconds": None, "release_p95_seconds": None,
                         "outside_tuning_gates": True, **replay(events, policy)})
    for path in args.policy:
        config = json.loads(path.read_text("utf-8-sig"))
        report_path = HERE / "artifacts" / config.get("acceptance", {}).get("source_report", "")
        gates = {}
        if report_path.is_file():
            selected = json.loads(report_path.read_text("utf-8"))["selected_candidate"]
            gates = {k: selected[k] for k in ("quiet_recall", "quiet_false_positive_rate", "exact_context_accuracy",
                                              "activation_p95_seconds", "release_p95_seconds")}
        rows.append({"policy": config["engine"], "config_file": path.name, "config_version": config["config_version"],
                     **gates, **replay(events, config["engine"])})
    rows.sort(key=lambda r: (r["activations"] + r["releases"], -(r["quiet_recall"] or 0), r["quiet_false_positive_rate"] or 1,
                             -(r["exact_context_accuracy"] or 0)))
    current = next(r for r in rows if r["policy"] == tuning["selected_candidate"]["policy"])
    configs = [r for r in rows if "config_file" in r]
    report = {"schema_version": "1.0", "audio": f"{args.minutes} min AIHub 464 meeting, 6 min meeting / 2 min quiet",
              # a meeting starts every 8 min; it is released only if its 6 min end before the stream does
              "ideal": {"activations": math.ceil(args.minutes / 8), "releases": max(0, math.ceil((args.minutes - 6) / 8))},
              "windows": len(events), "current_policy": current, "best": rows[0], "policy_files": configs,
              "candidates": rows}
    args.out.write_text(json.dumps(report, ensure_ascii=False, indent=1), "utf-8")
    print(json.dumps({"current": current, "best": rows[0], "policy_files": configs, "top5": rows[:5]},
                     ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
