"""PC replay and inspection commands for context fusion."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
import sys
import time
from uuid import uuid4

from .audio_pipeline import (
    DEFAULT_ACTIVITY_MODEL,
    DEFAULT_SOUND_MODEL,
    FusionInputDefaults,
    run_wav,
    summarize_frames,
)
from .deployment import build_release_candidate, verify_release_candidate
from .engine import ContextFusionEngine
from .evaluation import evaluate_manifest
from .models import ContextObservation
from .policy_config import ContextFusionPolicy
from .stream_fusion import FeatureStreamFusion
from .tuning import run_audio_policy_tuning
from .validation import validate_event, validate_sanitized_context


SERVICE_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCENARIO = SERVICE_ROOT / "scenarios" / "demo.jsonl"
DEFAULT_STREAM_SCENARIO = SERVICE_ROOT / "scenarios" / "async_features.jsonl"


def _read_jsonl(path: Path):
    if str(path) == "-":
        yield from _read_jsonl_handle(sys.stdin, Path("<stdin>"))
        return
    with path.open("r", encoding="utf-8-sig") as handle:
        yield from _read_jsonl_handle(handle, path)


def _read_jsonl_handle(handle, source: Path):
    for line_number, line in enumerate(handle, 1):
        if line.strip():
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"{source}:{line_number}: object required")
            yield value


def _print(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2))


def _float_list(value: str) -> tuple[float, ...]:
    try:
        result = tuple(float(item.strip()) for item in value.split(",") if item.strip())
    except ValueError as exc:
        raise argparse.ArgumentTypeError("comma-separated numbers required") from exc
    if not result:
        raise argparse.ArgumentTypeError("at least one number is required")
    return result


def _int_list(value: str) -> tuple[int, ...]:
    try:
        result = tuple(int(item.strip()) for item in value.split(",") if item.strip())
    except ValueError as exc:
        raise argparse.ArgumentTypeError("comma-separated integers required") from exc
    if not result or min(result) < 1:
        raise argparse.ArgumentTypeError("positive comma-separated integers required")
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="HomeMind context fusion")
    commands = parser.add_subparsers(dest="command", required=True)

    predict = commands.add_parser("predict", help="classify one minimized observation")
    predict.add_argument("json", help="one JSON object")

    replay = commands.add_parser("replay", help="replay JSONL observations through hysteresis")
    replay.add_argument("path", nargs="?", type=Path, default=DEFAULT_SCENARIO)
    replay.add_argument("--sanitized", action="store_true", help="emit the EXAONE projection")

    stream = commands.add_parser(
        "stream-replay",
        help="join asynchronous feature JSONL and emit events on audio windows",
    )
    stream.add_argument(
        "path",
        nargs="?",
        type=Path,
        default=DEFAULT_STREAM_SCENARIO,
        help="feature JSONL path, or - for a live stdin stream",
    )
    stream.add_argument("--sanitized", action="store_true", help="emit the EXAONE projection")
    stream.add_argument("--policy-config", type=Path, help="versioned fusion policy JSON")
    stream.add_argument("--events-jsonl", type=Path, help="write emitted contract events")
    stream.add_argument("--debug-jsonl", type=Path, help="write every ingest decision")

    audio = commands.add_parser(
        "audio-replay", help="run WAV -> YAMNet heads -> fused context events"
    )
    audio.add_argument("wav", type=Path)
    audio.add_argument("--activity-model", type=Path, default=DEFAULT_ACTIVITY_MODEL)
    audio.add_argument("--sound-model", type=Path, default=DEFAULT_SOUND_MODEL)
    audio.add_argument("--start-seconds", type=float)
    audio.add_argument("--end-seconds", type=float)
    audio.add_argument("--window-seconds", type=float, default=4.0)
    audio.add_argument("--hop-seconds", type=float, default=1.0)
    audio.add_argument("--max-windows", type=int)
    audio.add_argument(
        "--trailing-silence-seconds",
        type=float,
        default=0.0,
        help="append generated silence to verify automatic release",
    )
    audio.add_argument("--trace-id")
    audio.add_argument(
        "--presence-zone",
        choices=["BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"],
        default="UNKNOWN",
    )
    audio.add_argument("--presence-confidence", type=float, default=0.0)
    audio.add_argument("--turns-per-minute", type=float)
    audio.add_argument(
        "--turn-taking-method",
        choices=["SPEAKER_CHANGE", "SPEECH_BURST_PROXY", "UNKNOWN"],
        default="UNKNOWN",
    )
    for name in ("tv-on", "laptop-active", "headphones-connected"):
        audio.add_argument(
            f"--{name}",
            action=argparse.BooleanOptionalAction,
            default=None,
        )
    audio.add_argument(
        "--debug-jsonl",
        type=Path,
        help="write per-window predictions and latency; stdout remains contract-only",
    )
    audio.add_argument(
        "--events-jsonl",
        type=Path,
        help="write contract-only events for EXAONE/Pi 4 bridge replay",
    )
    audio.add_argument("--summary", type=Path, help="write privacy-safe evaluation summary")
    audio.add_argument("--policy-config", type=Path, help="versioned fusion policy JSON")

    benchmark = commands.add_parser("benchmark", help="measure fusion-only PC latency")
    benchmark.add_argument("path", nargs="?", type=Path, default=DEFAULT_SCENARIO)
    benchmark.add_argument("--runs", type=int, default=1000)
    benchmark.add_argument(
        "--report", type=Path, default=SERVICE_ROOT / "artifacts" / "benchmark-pc.json"
    )

    evaluate = commands.add_parser(
        "evaluate-audio", help="evaluate fused context on real manifest segments"
    )
    evaluate.add_argument("--manifest", required=True, type=Path)
    evaluate.add_argument("--activity-model", type=Path, default=DEFAULT_ACTIVITY_MODEL)
    evaluate.add_argument("--sound-model", type=Path, default=DEFAULT_SOUND_MODEL)
    evaluate.add_argument("--split", default="validation")
    evaluate.add_argument("--per-label", type=int, default=5)
    evaluate.add_argument("--seed", type=int, default=42)
    evaluate.add_argument(
        "--report",
        type=Path,
        default=SERVICE_ROOT / "artifacts" / "real-audio-evaluation.json",
    )

    tune = commands.add_parser(
        "tune-audio-policy",
        help="calibrate quiet thresholds on group-disjoint validation subsets",
    )
    tune.add_argument("--manifest", required=True, type=Path)
    tune.add_argument("--activity-model", type=Path, default=DEFAULT_ACTIVITY_MODEL)
    tune.add_argument("--sound-model", type=Path, default=DEFAULT_SOUND_MODEL)
    tune.add_argument("--calibration-per-label", type=int, default=100)
    tune.add_argument("--holdout-per-label", type=int, default=100)
    tune.add_argument("--seed", type=int, default=20260903)
    tune.add_argument("--calibration-split", default="validation")
    tune.add_argument("--holdout-split")
    tune.add_argument("--config-version", default="audio-policy-v5.0.0")
    tune.add_argument(
        "--quiet-thresholds",
        type=_float_list,
        default=(0.0, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90),
    )
    tune.add_argument(
        "--meeting-tv-margins",
        type=_float_list,
        default=(0.0, 0.025, 0.05, 0.075, 0.10, 0.15, 0.20, 0.25),
    )
    tune.add_argument("--activation-updates", type=_int_list, default=(2, 3, 4, 5))
    tune.add_argument("--release-updates", type=_int_list, default=(2, 3, 4))
    tune.add_argument(
        "--max-release-p95",
        type=float,
        default=8.0,
        help="release-delay gate in seconds (8.0 = CTX gate; the demo hold policy uses 20.0)",
    )
    tune.add_argument(
        "--report",
        type=Path,
        default=SERVICE_ROOT / "artifacts" / "audio-policy-tuning-v5.json",
    )
    tune.add_argument(
        "--config-out",
        type=Path,
        default=SERVICE_ROOT / "config" / "audio-policy-v5.json",
    )

    package = commands.add_parser(
        "package-release", help="build a hash-verified Pi 5 staging bundle"
    )
    package.add_argument("--output", required=True, type=Path)
    package.add_argument("--activity-model", required=True, type=Path)
    package.add_argument("--sound-model", required=True, type=Path)
    package.add_argument("--policy-config", required=True, type=Path)
    package.add_argument("--training-metrics", required=True, type=Path)
    package.add_argument("--tuning-report", required=True, type=Path)
    package.add_argument("--evaluation-report", type=Path)
    package.add_argument("--runtime-wheel", type=Path, action="append", default=[])
    package.add_argument("--rollback-activity-model", type=Path)
    package.add_argument("--rollback-sound-model", type=Path)
    package.add_argument("--rollback-metrics", type=Path)
    package.add_argument("--package-version")
    package.add_argument(
        "--rollback-model-version", default="public-balanced-v5-window4s"
    )

    verify = commands.add_parser(
        "verify-release", help="verify every SHA-256 and size in a release bundle"
    )
    verify.add_argument("package", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "predict":
        observation = ContextObservation.from_mapping(json.loads(args.json))
        result = ContextFusionEngine.classify_once(observation)
        _print(
            {
                "context_type": result.context_type,
                "confidence": result.confidence,
                "evidence": result.evidence,
                "scores": result.scores,
            }
        )
    elif args.command == "replay":
        engine = ContextFusionEngine()
        for value in _read_jsonl(args.path):
            event = engine.update(ContextObservation.from_mapping(value))
            validate_event(event)
            if args.sanitized:
                event = engine.sanitized_context(event)
                validate_sanitized_context(event)
            print(json.dumps(event, ensure_ascii=False), flush=True)
    elif args.command == "stream-replay":
        policy = (
            ContextFusionPolicy.from_json(args.policy_config)
            if args.policy_config
            else ContextFusionPolicy()
        )
        fusion = FeatureStreamFusion(engine=policy.engine())
        event_handle = None
        debug_handle = None
        if args.events_jsonl:
            args.events_jsonl.parent.mkdir(parents=True, exist_ok=True)
            event_handle = args.events_jsonl.open("w", encoding="utf-8")
        if args.debug_jsonl:
            args.debug_jsonl.parent.mkdir(parents=True, exist_ok=True)
            debug_handle = args.debug_jsonl.open("w", encoding="utf-8")
        try:
            for value in _read_jsonl(args.path):
                result = fusion.ingest(value)
                if debug_handle:
                    debug_handle.write(
                        json.dumps(result.debug_record(), ensure_ascii=False) + "\n"
                    )
                    debug_handle.flush()
                if result.context_event is None:
                    print(
                        f"input={result.event_type} status={result.reason}",
                        file=sys.stderr,
                        flush=True,
                    )
                    continue
                contract_event = result.context_event
                validate_event(contract_event)
                output = contract_event
                if args.sanitized:
                    output = ContextFusionEngine.sanitized_context(contract_event)
                    validate_sanitized_context(output)
                print(json.dumps(output, ensure_ascii=False), flush=True)
                if event_handle:
                    event_handle.write(json.dumps(output, ensure_ascii=False) + "\n")
                    event_handle.flush()
                print(
                    f"input=AUDIO_CONTEXT status=fused "
                    f"context={contract_event['context_type']} "
                    f"quiet_required={contract_event['quiet_required']}",
                    file=sys.stderr,
                    flush=True,
                )
        finally:
            if event_handle:
                event_handle.close()
            if debug_handle:
                debug_handle.close()
    elif args.command == "audio-replay":
        if args.max_windows is not None and args.max_windows < 1:
            raise ValueError("max_windows must be positive")
        defaults = FusionInputDefaults(
            presence_zone=args.presence_zone,
            presence_confidence=args.presence_confidence,
            turns_per_minute=args.turns_per_minute,
            turn_taking_method=args.turn_taking_method,
            tv_on=args.tv_on,
            laptop_active=args.laptop_active,
            headphones_connected=args.headphones_connected,
        )
        debug_handle = None
        events_handle = None
        frames = []
        if args.debug_jsonl:
            args.debug_jsonl.parent.mkdir(parents=True, exist_ok=True)
            debug_handle = args.debug_jsonl.open("w", encoding="utf-8")
        if args.events_jsonl:
            args.events_jsonl.parent.mkdir(parents=True, exist_ok=True)
            events_handle = args.events_jsonl.open("w", encoding="utf-8")
        try:
            for frame in run_wav(
                args.wav,
                activity_model=args.activity_model,
                sound_model=args.sound_model,
                start_seconds=args.start_seconds,
                end_seconds=args.end_seconds,
                window_seconds=args.window_seconds,
                hop_seconds=args.hop_seconds,
                trace_id=args.trace_id or f"trace-{uuid4()}",
                defaults=defaults,
                max_windows=args.max_windows,
                trailing_silence_seconds=args.trailing_silence_seconds,
                engine=(
                    ContextFusionPolicy.from_json(args.policy_config).engine()
                    if args.policy_config
                    else None
                ),
            ):
                frames.append(frame)
                print(json.dumps(frame.context_event, ensure_ascii=False), flush=True)
                if events_handle:
                    events_handle.write(
                        json.dumps(frame.context_event, ensure_ascii=False) + "\n"
                    )
                    events_handle.flush()
                if debug_handle:
                    debug_handle.write(
                        json.dumps(frame.debug_record(), ensure_ascii=False) + "\n"
                    )
                    debug_handle.flush()
                print(
                    f"window={frame.window_index} "
                    f"audio={frame.activity_prediction['label']} "
                    f"({frame.activity_prediction['confidence']:.3f}) "
                    f"context={frame.context_event['context_type']} "
                    f"quiet_required={frame.context_event['quiet_required']} "
                    f"latency={frame.latency_ms:.1f}ms",
                    file=sys.stderr,
                    flush=True,
                )
        finally:
            if debug_handle:
                debug_handle.close()
            if events_handle:
                events_handle.close()
        if args.summary:
            summary = summarize_frames(frames, window_seconds=args.window_seconds)
            args.summary.parent.mkdir(parents=True, exist_ok=True)
            args.summary.write_text(
                json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
    elif args.command == "benchmark":
        observations = [ContextObservation.from_mapping(value) for value in _read_jsonl(args.path)]
        if not observations or args.runs < 1:
            raise ValueError("at least one observation and one run are required")
        timings: list[float] = []
        engine = ContextFusionEngine()
        for index in range(args.runs):
            if index and index % len(observations) == 0:
                engine = ContextFusionEngine()
            started = time.perf_counter_ns()
            engine.update(observations[index % len(observations)])
            timings.append((time.perf_counter_ns() - started) / 1_000_000)
        timings.sort()
        percentile = lambda p: timings[min(len(timings) - 1, int(len(timings) * p))]
        report = {
            "platform": "pc",
            "runs": args.runs,
            "mean_ms": round(statistics.fmean(timings), 4),
            "p50_ms": round(percentile(0.50), 4),
            "p95_ms": round(percentile(0.95), 4),
            "p99_ms": round(percentile(0.99), 4),
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        _print(report)
    elif args.command == "evaluate-audio":
        report = evaluate_manifest(
            args.manifest,
            activity_model=args.activity_model,
            sound_model=args.sound_model,
            split=args.split,
            per_label=args.per_label,
            seed=args.seed,
            calibration_split=args.calibration_split,
            holdout_split=args.holdout_split,
        )
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        _print(report)
    elif args.command == "tune-audio-policy":
        report = run_audio_policy_tuning(
            args.manifest,
            activity_model=args.activity_model,
            sound_model=args.sound_model,
            calibration_per_label=args.calibration_per_label,
            holdout_per_label=args.holdout_per_label,
            seed=args.seed,
            calibration_split=args.calibration_split,
            holdout_split=args.holdout_split,
            quiet_thresholds=args.quiet_thresholds,
            meeting_tv_margins=args.meeting_tv_margins,
            activation_updates=args.activation_updates,
            release_updates=args.release_updates,
            maximum_release_p95_seconds=args.max_release_p95,
            progress=lambda message: print(message, file=sys.stderr, flush=True),
        )
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        config = {
            "schema_version": "1.0",
            "config_version": args.config_version,
            "engine": report["selected_candidate"]["policy"],
            "acceptance": {
                "calibration_passed": report["calibration"]["gate"]["passed"],
                "holdout_passed": report["holdout"]["gate"]["passed"],
                "source_report": args.report.name,
            },
        }
        args.config_out.parent.mkdir(parents=True, exist_ok=True)
        args.config_out.write_text(
            json.dumps(config, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        _print(
            {
                "report": str(args.report.resolve()),
                "config": str(args.config_out.resolve()),
                "selected_candidate": report["selected_candidate"],
                "calibration_gate": report["calibration"]["gate"],
                "holdout_gate": report["holdout"]["gate"],
            }
        )
    elif args.command == "package-release":
        manifest = build_release_candidate(
            args.output,
            activity_model=args.activity_model,
            sound_model=args.sound_model,
            policy_config=args.policy_config,
            training_metrics=args.training_metrics,
            tuning_report=args.tuning_report,
            evaluation_report=args.evaluation_report,
            runtime_wheels=args.runtime_wheel,
            rollback_activity_model=args.rollback_activity_model,
            rollback_sound_model=args.rollback_sound_model,
            rollback_metrics=args.rollback_metrics,
            package_version=args.package_version,
            rollback_model_version=args.rollback_model_version,
        )
        _print(
            {
                "package": str(args.output.resolve()),
                "activation": manifest["activation"],
                "files": len(manifest["files"]),
            }
        )
    elif args.command == "verify-release":
        result = verify_release_candidate(args.package)
        _print(result)
        if not result["valid"]:
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
