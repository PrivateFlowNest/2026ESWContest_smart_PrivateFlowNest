"""CLI for the PC llama.cpp baseline and later Pi 5 adapter."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from .audio_bridge import AudioContextBridge, HttpProposalSink
from .client import ExaoneClient
from .deployment import build_release_candidate, verify_release_candidate
from .e2e_simulation import load_e2e_cases, run_e2e_gate
from .evaluation import check_targets, evaluate_cases, load_cases
from .validation import load_json_object


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="homemind-exaone")
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--model", default="exaone-4.0-1.2b")
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    parser.add_argument("--max-tokens", type=int, default=256)
    parser.add_argument("--retries", type=int, default=1)
    parser.add_argument(
        "--guardrails",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="deterministic safety/command guard; disable only for raw-model evaluation",
    )
    parser.add_argument(
        "--few-shots",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="include prompt examples (fine-tuned model evaluation may disable them)",
    )
    parser.add_argument("--schema", type=Path, default=None)
    parser.add_argument("--capabilities", type=Path, default=None)
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("health", help="Check the llama.cpp server")

    interpret = commands.add_parser("interpret", help="Create one safe Task proposal")
    interpret.add_argument("utterance")
    interpret.add_argument(
        "--context",
        default="{}",
        help="Sanitized context JSON object; raw media must never be included",
    )
    interpret.add_argument(
        "--debug",
        action="store_true",
        help="Include latency, retries, and validation diagnostics",
    )

    evaluate = commands.add_parser("eval", help="Run a JSONL acceptance suite")
    evaluate.add_argument(
        "--cases", type=Path, default=Path("data/smoke-cases.jsonl")
    )
    evaluate.add_argument(
        "--report", type=Path, default=Path("artifacts/smoke-report.json")
    )
    evaluate.add_argument(
        "--targets",
        type=Path,
        default=None,
        help="JSON acceptance thresholds; exits non-zero when any target is missed",
    )
    evaluate.add_argument(
        "--summary-only",
        action="store_true",
        help="print metrics without the per-case result list",
    )

    bridge = commands.add_parser(
        "bridge-audio",
        help="Read stabilized audio JSONL, ask EXAONE on transitions, and forward to Pi 4",
    )
    bridge.add_argument(
        "--events",
        type=Path,
        default=None,
        help="Read JSONL from a file instead of stdin",
    )

    e2e = commands.add_parser(
        "e2e-sim",
        help="Run EXAONE through a spreadsheet-ID Pi 4/controller simulation",
    )
    e2e.add_argument(
        "--cases", type=Path, default=Path("data/pc-e2e-cases-v1.jsonl")
    )
    e2e.add_argument(
        "--inventory", type=Path, default=Path("config/hardware-inventory.json")
    )
    e2e.add_argument(
        "--report", type=Path, default=Path("artifacts/pc-e2e-report.json")
    )

    package = commands.add_parser(
        "package-release", help="Build a hash-pinned Pi 5 staging bundle"
    )
    package.add_argument("--output", type=Path, required=True)
    package.add_argument("--model-file", type=Path, required=True)
    package.add_argument("--inventory", type=Path, required=True)
    package.add_argument("--acceptance-report", type=Path, required=True)
    package.add_argument("--e2e-report", type=Path, required=True)
    package.add_argument(
        "--bridge-report", type=Path, default=None,
        help="legacy audio-bridge report (optional; quiet transitions now go through perception)",
    )
    package.add_argument("--runtime-wheel", type=Path)
    package.add_argument("--package-version", default="exaone-edge-v1")

    verify_package = commands.add_parser(
        "verify-package", help="Verify all release bundle hashes"
    )
    verify_package.add_argument("package", type=Path)
    bridge.add_argument(
        "--pi4-url",
        default=None,
        help="Pi 4 policy endpoint; if omitted, envelopes are printed as JSONL",
    )
    bridge.add_argument(
        "--output",
        type=Path,
        default=None,
        help="also save validated proposal envelopes as JSONL",
    )
    bridge.add_argument("--pi4-timeout-seconds", type=float, default=10.0)
    bridge.add_argument("--retry-cooldown-seconds", type=float, default=15.0)
    bridge.add_argument(
        "--warmup",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="warm the LLM prompt cache before accepting context transitions",
    )
    bridge.add_argument(
        "--presence-zone",
        choices=["BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"],
        default="DESK",
    )
    return parser


def _client(args: argparse.Namespace) -> ExaoneClient:
    return ExaoneClient(
        base_url=args.base_url,
        model=args.model,
        schema=load_json_object(args.schema) if args.schema else None,
        capabilities=(
            load_json_object(args.capabilities) if args.capabilities else None
        ),
        timeout_seconds=args.timeout_seconds,
        max_tokens=args.max_tokens,
        retries=args.retries,
        api_key=os.environ.get("HOMEMIND_LLM_API_KEY"),
        enable_guardrails=args.guardrails,
        include_few_shots=args.few_shots,
    )


def main() -> None:
    args = _parser().parse_args()
    client = _client(args)
    if args.command == "health":
        print(json.dumps(client.health(), ensure_ascii=False, indent=2))
        return
    if args.command == "interpret":
        context = json.loads(args.context)
        if not isinstance(context, dict):
            raise ValueError("context must be a JSON object")
        result = client.interpret(args.utterance, context=context)
        output = result.as_dict(include_raw=True) if args.debug else result.proposal
        print(json.dumps(output, ensure_ascii=False, indent=2))
        if not result.accepted:
            raise SystemExit(2)
        return
    if args.command == "bridge-audio":
        output_handle = None
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            output_handle = args.output.open("w", encoding="utf-8")
        if args.pi4_url:
            sink = HttpProposalSink(
                args.pi4_url,
                timeout_seconds=args.pi4_timeout_seconds,
                api_key=os.environ.get("HOMEMIND_PI4_API_KEY"),
            )
        else:
            def sink(envelope):
                serialized = json.dumps(
                    envelope, ensure_ascii=False, separators=(",", ":")
                )
                print(serialized, flush=True)
                if output_handle:
                    output_handle.write(serialized + "\n")
                    output_handle.flush()
        bridge = AudioContextBridge(
            client,
            sink,
            presence_zone=args.presence_zone,
            retry_cooldown_seconds=args.retry_cooldown_seconds,
        )
        if args.warmup:
            warmup = client.interpret(
                "시스템 감지 이벤트: 회의가 시작되어 조용한 환경이 필요함.",
                context={
                    "presence_zone": "UNKNOWN",
                    "activity": "MEETING",
                    "audio_context": "IN_PERSON_MEETING",
                    "quiet_required": True,
                    "confidence": 0.9,
                },
            )
            print(
                f"bridge warmup accepted={warmup.accepted} "
                f"latency={warmup.latency_ms:.1f}ms",
                file=sys.stderr,
                flush=True,
            )
        input_handle = args.events.open("r", encoding="utf-8") if args.events else sys.stdin
        try:
            for line_number, line in enumerate(input_handle, start=1):
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                    if not isinstance(event, dict):
                        raise ValueError("JSON value must be an object")
                    outcome = bridge.handle(event)
                except (json.JSONDecodeError, ValueError) as exc:
                    print(
                        f"bridge line={line_number} status=invalid_json error={exc}",
                        file=sys.stderr,
                        flush=True,
                    )
                    continue
                if outcome.status not in {
                    "forwarded",
                    "ignored_heartbeat",
                    "ignored_sound_event",
                    "baseline_initialized",
                }:
                    detail = "; ".join(outcome.errors)
                    print(
                        f"bridge line={line_number} status={outcome.status} "
                        f"transition={outcome.transition or '-'} error={detail or '-'}",
                        file=sys.stderr,
                        flush=True,
                    )
                elif outcome.status == "forwarded":
                    print(
                        f"bridge line={line_number} status=forwarded "
                        f"transition={outcome.transition}",
                        file=sys.stderr,
                        flush=True,
                    )
        finally:
            if args.events:
                input_handle.close()
            if output_handle:
                output_handle.close()
        return

    if args.command == "e2e-sim":
        capabilities = (
            load_json_object(args.capabilities)
            if args.capabilities
            else load_json_object(Path("config/device-capabilities.json"))
        )
        inventory = load_json_object(args.inventory)
        report = run_e2e_gate(
            client,
            load_e2e_cases(args.cases),
            capabilities=capabilities,
            inventory=inventory,
        )
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if not report["gate_passed"]:
            raise SystemExit(4)
        return

    if args.command == "package-release":
        capabilities_path = args.capabilities or Path("config/device-capabilities.json")
        contract_root = Path(__file__).resolve().parents[4] / "contracts"
        internal = contract_root / "rpi5-internal"
        pfn = contract_root / "pfn.v1"
        manifest = build_release_candidate(
            args.output,
            model=args.model_file,
            capabilities=capabilities_path,
            inventory=args.inventory,
            schemas=[
                internal / "task-proposal.schema.json",
                internal / "activity-context.schema.json",
                pfn / "llm-request.schema.json",
                pfn / "llm-result.schema.json",
                pfn / "task-request.schema.json",
                pfn / "device-control.schema.json",
                pfn / "ai-action-map.json",
            ],
            acceptance_report=args.acceptance_report,
            e2e_report=args.e2e_report,
            bridge_report=args.bridge_report,
            runtime_wheel=args.runtime_wheel,
            package_version=args.package_version,
        )
        print(json.dumps(manifest, ensure_ascii=False, indent=2))
        return

    if args.command == "verify-package":
        result = verify_release_candidate(args.package)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if not result["valid"]:
            raise SystemExit(5)
        return

    cases = load_cases(args.cases)
    report = evaluate_cases(
        client,
        cases,
        progress=lambda message: print(message, file=sys.stderr, flush=True),
    )
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    displayed_report = (
        {key: value for key, value in report.items() if key != "results"}
        if args.summary_only
        else report
    )
    print(json.dumps(displayed_report, ensure_ascii=False, indent=2))
    if args.targets:
        failures = check_targets(report, load_json_object(args.targets))
        if failures:
            for failure in failures:
                print(f"acceptance target failed: {failure}", file=sys.stderr)
            raise SystemExit(3)


if __name__ == "__main__":
    main()
