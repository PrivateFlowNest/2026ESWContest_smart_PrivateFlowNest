"""Command line interface for HomeMind local Korean ASR."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from .backends import create_backend
from .cascade import cascade_events
from .dataset import build_aihub_manifest, load_jsonl, write_jsonl
from .evaluation import evaluate, evaluate_acceptance


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="homemind-stt")
    commands = root.add_subparsers(dest="command", required=True)
    manifest = commands.add_parser("manifest-aihub")
    manifest.add_argument("--labels", type=Path, required=True)
    manifest.add_argument("--audio-root", type=Path, required=True)
    manifest.add_argument("--output", type=Path, required=True)
    manifest.add_argument("--limit", type=int, default=100)
    manifest.add_argument("--seed", type=int, default=20260903)
    manifest.add_argument("--keywords", default="")
    manifest.add_argument("--domains", default="")
    manifest.add_argument("--split-name", choices=["training", "validation"], default="validation")
    manifest.add_argument("--audio-variant", choices=["S", "N"], default="S")
    cascade = commands.add_parser(
        "cascade", help="emit Vosk partials followed by one accurate final transcript"
    )
    cascade.add_argument("wav", type=Path)
    cascade.add_argument("--partial-model-dir", type=Path, required=True)
    cascade.add_argument(
        "--final-backend", choices=["sherpa", "whisper", "whisper-server"],
        required=True,
    )
    cascade.add_argument("--final-model-dir", type=Path)
    cascade.add_argument("--final-model-file", type=Path)
    cascade.add_argument("--server-url")
    cascade.add_argument("--model-version")
    cascade.add_argument("--threads", type=int, default=4)
    for name in ("transcribe", "stream", "eval"):
        command = commands.add_parser(name)
        command.add_argument(
            "--backend",
            choices=["vosk", "sherpa", "whisper", "whisper-server"],
            required=True,
        )
        command.add_argument("--model-dir", type=Path)
        command.add_argument("--model-file", type=Path)
        command.add_argument("--server-url")
        command.add_argument("--model-version")
        command.add_argument("--threads", type=int, default=4)
        if name in {"transcribe", "stream"}:
            command.add_argument("wav", type=Path)
        else:
            command.add_argument("--manifest", type=Path, required=True)
            command.add_argument("--report", type=Path, required=True)
            command.add_argument("--targets", type=Path)
    return root


def main() -> None:
    args = parser().parse_args()
    if args.command == "manifest-aihub":
        keywords = tuple(item.strip() for item in args.keywords.split(",") if item.strip())
        domains = tuple(item.strip() for item in args.domains.split(",") if item.strip())
        rows = build_aihub_manifest(
            args.labels, args.audio_root,
            limit=None if args.limit == 0 else args.limit,
            seed=args.seed, keywords=keywords, audio_variant=args.audio_variant,
            domains=domains, split_name=args.split_name,
        )
        write_jsonl(rows, args.output)
        print(json.dumps({"output": str(args.output), "rows": len(rows)}, ensure_ascii=False))
        if not rows:
            raise SystemExit(2)
        return
    if args.command == "cascade":
        partial_backend = create_backend("vosk", args.partial_model_dir)
        final_backend = create_backend(
            args.final_backend,
            args.final_model_dir,
            threads=args.threads,
            model_file=args.final_model_file,
            server_url=args.server_url,
            model_version=args.model_version,
        )
        for event in cascade_events(partial_backend, final_backend, args.wav):
            print(json.dumps(event, ensure_ascii=False), flush=True)
        return
    backend = create_backend(
        args.backend, args.model_dir, threads=args.threads,
        model_file=args.model_file, server_url=args.server_url,
        model_version=args.model_version,
    )
    if args.command == "transcribe":
        result = backend.transcribe(args.wav)
        print(json.dumps({"text": result.text, "model_version": result.model_version}, ensure_ascii=False))
        return
    if args.command == "stream":
        if not hasattr(backend, "events"):
            raise ValueError(f"{args.backend} backend does not expose partial updates")
        for event in backend.events(args.wav):
            print(json.dumps(event, ensure_ascii=False), flush=True)
        return
    report = evaluate(
        backend, load_jsonl(args.manifest),
        progress=lambda value: print(value, file=sys.stderr, flush=True),
    )
    if args.targets:
        target_config = json.loads(args.targets.read_text(encoding="utf-8"))
        report["pc_acceptance"] = evaluate_acceptance(
            report, target_config["pc_acceptance"]
        )
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    summary = {key: value for key, value in report.items() if key != "results"}
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
