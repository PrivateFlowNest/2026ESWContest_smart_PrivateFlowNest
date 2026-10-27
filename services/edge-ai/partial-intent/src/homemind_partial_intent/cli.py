"""Command-line interface for the PC-first partial-intent baseline."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from .dataset import build_seed_dataset
from .runtime import IntentRuntime, StreamIntentGate
from .training import benchmark_model, evaluate_model, train_model
from .validation import validate_event


SERVICE_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATA = SERVICE_ROOT / "data" / "partial-intent-v2"
DEFAULT_MODEL = SERVICE_ROOT / "artifacts" / "partial-intent-v2.onnx"


def _print(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="HomeMind partial-intent classifier")
    subparsers = parser.add_subparsers(dest="command", required=True)

    dataset = subparsers.add_parser("dataset", help="build the grouped Korean seed dataset")
    dataset.add_argument("--output", type=Path, default=DEFAULT_DATA)

    train = subparsers.add_parser("train", help="train and export ONNX")
    train.add_argument("--data", type=Path, default=DEFAULT_DATA)
    train.add_argument("--output", type=Path, default=DEFAULT_MODEL)

    evaluate = subparsers.add_parser("evaluate", help="evaluate the held-out split")
    evaluate.add_argument("--data", type=Path, default=DEFAULT_DATA)
    evaluate.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    evaluate.add_argument("--report", type=Path, default=SERVICE_ROOT / "artifacts" / "metrics.json")
    evaluate.add_argument("--input-file", type=Path)

    predict = subparsers.add_parser("predict", help="predict one partial utterance")
    predict.add_argument("text")
    predict.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    predict.add_argument("--final", action="store_true")
    predict.add_argument("--trace-id")

    stream = subparsers.add_parser("stream", help="read JSONL streaming-ASR updates from stdin")
    stream.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    stream.add_argument("--confirmation-updates", type=int, default=2)
    stream.add_argument("--threshold", type=float)

    benchmark = subparsers.add_parser("benchmark", help="measure PC end-to-end latency")
    benchmark.add_argument("--data", type=Path, default=DEFAULT_DATA)
    benchmark.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    benchmark.add_argument("--runs", type=int, default=1000)
    benchmark.add_argument("--report", type=Path, default=SERVICE_ROOT / "artifacts" / "benchmark-pc.json")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "dataset":
        _print(build_seed_dataset(args.output))
    elif args.command == "train":
        _print(train_model(args.data, args.output))
    elif args.command == "evaluate":
        _print(evaluate_model(args.data, args.model, args.report, args.input_file))
    elif args.command == "predict":
        event = IntentRuntime(args.model).predict(
            args.text, is_final=args.final, trace_id=args.trace_id
        )
        validate_event(event)
        _print(event)
    elif args.command == "stream":
        gate = StreamIntentGate(
            IntentRuntime(args.model),
            confirmation_updates=args.confirmation_updates,
            threshold=args.threshold,
        )
        for line in sys.stdin:
            if not line.strip():
                continue
            update = json.loads(line)
            event = gate.update(
                update["text"],
                is_final=bool(update.get("is_final", False)),
                # The ASR privacy contract calls the shared correlation value
                # utterance_id. Keep accepting trace_id for older producers.
                trace_id=update.get("trace_id") or update.get("utterance_id"),
            )
            validate_event(event)
            print(json.dumps(event, ensure_ascii=False), flush=True)
    elif args.command == "benchmark":
        result = benchmark_model(args.data, args.model, runs=args.runs)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        _print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
