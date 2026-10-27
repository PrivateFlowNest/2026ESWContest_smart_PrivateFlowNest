"""Command-line entry point for the PC baseline."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import sys
from pathlib import Path
from uuid import uuid4

from .archive_processing import (
    DELETE_CONFIRMATION,
    extract_selected_archives,
    scan_archives,
    select_archives,
    verify_selected_archives,
)
from .audio import load_wav_16k_mono
from .aihub_manifest import build_aihub_manifest
from .dataset import iter_manifest
from .ensemble import build_activity_ensemble
from .fsd50k_manifest import build_fsd50k_manifest
from .manifest_ops import (
    build_balanced_manifest,
    merge_manifests,
    reserve_validation_groups,
    windowize_manifest,
)
from .models import prediction_to_event, prediction_to_feature_event
from .realtime import (
    FusionConfig,
    HybridAudioPredictor,
    LiveAudioContextEngine,
    iter_microphone_windows,
    iter_waveform_windows,
    run_windows,
)
from .recording import append_manifest, list_input_devices, record_clip, save_clip
from .readiness import assess_final_readiness
from .realised_manifest import build_realised_manifest
from .training import benchmark_batch_sizes, predict_file, train_from_manifest
from .tau_manifest import build_tau_manifest
from .yamnet import (
    FEATURE_VERSION_MEAN_VAD_V1,
    FEATURE_VERSIONS,
    YamnetEmbeddingExtractor,
)


def _batch_size(value: str) -> int | str:
    if value.lower() == "auto":
        return "auto"
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("batch size must be positive or 'auto'")
    return parsed


def _batch_sizes(value: str) -> tuple[int, ...]:
    try:
        sizes = tuple(int(item.strip()) for item in value.split(",") if item.strip())
    except ValueError as exc:
        raise argparse.ArgumentTypeError("batch sizes must be comma-separated integers") from exc
    if not sizes or any(size < 1 for size in sizes):
        raise argparse.ArgumentTypeError("batch sizes must all be positive")
    return sizes


def _positive_floats(value: str) -> tuple[float, ...]:
    try:
        values = tuple(float(item.strip()) for item in value.split(",") if item.strip())
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "values must be comma-separated positive numbers"
        ) from exc
    if not values or any(item <= 0 for item in values):
        raise argparse.ArgumentTypeError("values must all be positive")
    return values


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="homemind-audio")
    commands = parser.add_subparsers(dest="command", required=True)

    train = commands.add_parser("train", help="Train heads from a CSV manifest")
    train.add_argument("--manifest", required=True, type=Path)
    train.add_argument("--output", required=True, type=Path)
    train.add_argument(
        "--cache",
        type=Path,
        default=None,
        help="SQLite embedding cache used to resume and accelerate retraining",
    )
    train.add_argument(
        "--skip-file-check",
        action="store_true",
        help="Skip the initial path scan when the manifest was already verified",
    )
    train.add_argument(
        "--batch-size",
        type=_batch_size,
        default="auto",
        help="YAMNet waveform batch size, or auto to benchmark 1/2/4/8/16",
    )
    train.add_argument(
        "--prefetch-workers",
        type=int,
        default=2,
        help="CPU workers that read/resample upcoming audio (2 is HDD-friendly)",
    )
    train.add_argument(
        "--prefetch-batches",
        type=int,
        default=2,
        help="Maximum batches read ahead while the GPU is running",
    )
    train.add_argument(
        "--benchmark-samples",
        type=int,
        default=32,
        help="Audio clips used by automatic batch-size selection",
    )
    train.add_argument(
        "--feature-version",
        choices=FEATURE_VERSIONS,
        default=FEATURE_VERSION_MEAN_VAD_V1,
        help="Frozen YAMNet pooling definition stored with the trained model",
    )
    train.add_argument(
        "--tune-classifier",
        action="store_true",
        help="Select logistic-regression settings using validation Macro-F1",
    )
    train.add_argument(
        "--classifier-c-values",
        type=_positive_floats,
        default=(0.03, 0.1, 0.3, 1.0, 3.0, 10.0),
        help="Comma-separated LogisticRegression C candidates",
    )
    train.add_argument(
        "--cost-sensitive-activity",
        action="store_true",
        help="Penalize TV_MEDIA to IN_PERSON_MEETING errors during activity selection",
    )
    train.add_argument(
        "--activity-cost-profile",
        choices=["search", "recall-buffer"],
        default="search",
        help="Search all activity costs or fit the validation-selected recall-buffer profile",
    )
    train.add_argument(
        "--head",
        choices=["both", "activity", "sound_event"],
        default="both",
        help="Train both heads or only one head",
    )

    benchmark = commands.add_parser(
        "benchmark", help="Benchmark equivalent YAMNet batch sizes on this machine"
    )
    benchmark.add_argument("--manifest", required=True, type=Path)
    benchmark.add_argument("--samples", type=int, default=32)
    benchmark.add_argument(
        "--batch-sizes", type=_batch_sizes, default=(1, 2, 4, 8, 16)
    )
    benchmark.add_argument("--prefetch-workers", type=int, default=2)
    benchmark.add_argument(
        "--feature-version",
        choices=FEATURE_VERSIONS,
        default=FEATURE_VERSION_MEAN_VAD_V1,
    )
    benchmark.add_argument("--skip-file-check", action="store_true")
    benchmark.add_argument("--output", type=Path, default=None)

    predict = commands.add_parser("predict", help="Predict one WAV file")
    predict.add_argument("wav", type=Path)
    predict.add_argument("--model", required=True, type=Path)
    predict.add_argument("--trace-id", default=None)

    ensemble = commands.add_parser(
        "ensemble-activity",
        help="Blend compatible activity classifiers without rerunning YAMNet",
    )
    ensemble.add_argument("--model", action="append", required=True, type=Path)
    ensemble.add_argument("--weights", required=True, type=_positive_floats)
    ensemble.add_argument("--output", required=True, type=Path)
    ensemble.add_argument(
        "--model-version",
        default="yamnet-temporal-cost-ensemble-v0.8.0",
    )

    live = commands.add_parser(
        "live",
        help="Run stabilized v3 activity + v4 sound inference from a mic or WAV",
    )
    live.add_argument(
        "--activity-model",
        type=Path,
        default=Path("artifacts/public-balanced-v3/activity.joblib"),
    )
    live.add_argument(
        "--sound-model",
        type=Path,
        default=Path("artifacts/public-balanced-v4-temporal/sound_event.joblib"),
    )
    live.add_argument(
        "--wav",
        type=Path,
        default=None,
        help="Replay a WAV/FLAC instead of opening a microphone",
    )
    live.add_argument("--device", default=None, help="Input device index or name")
    live.add_argument("--window-seconds", type=float, default=4.0)
    live.add_argument("--hop-seconds", type=float, default=1.0)
    live.add_argument("--activity-threshold", type=float, default=0.65)
    live.add_argument("--enter-count", type=int, default=3)
    live.add_argument("--exit-count", type=int, default=2)
    live.add_argument("--sound-threshold", type=float, default=0.80)
    live.add_argument("--sound-confirm-count", type=int, default=2)
    live.add_argument("--sound-cooldown-seconds", type=float, default=8.0)
    live.add_argument("--heartbeat-seconds", type=float, default=5.0)
    live.add_argument("--context-ttl-seconds", type=int, default=10)
    live.add_argument(
        "--max-windows",
        type=int,
        default=None,
        help="Stop after this many windows (useful for PC smoke tests)",
    )
    live.add_argument("--trace-id", default=None)
    live.add_argument(
        "--feature-events",
        action="store_true",
        help="emit every raw activity window using the context-fusion input contract",
    )

    record = commands.add_parser("record", help="Record and label one microphone clip")
    record.add_argument("--head", choices=["activity", "sound_event"], required=True)
    record.add_argument("--label", required=True)
    record.add_argument("--seconds", type=float, default=8.0)
    record.add_argument("--device", default=None, help="Input device index or name")
    record.add_argument("--output", required=True, type=Path)
    record.add_argument("--manifest", type=Path, default=Path("data/manifest.csv"))
    record.add_argument(
        "--split", choices=["train", "validation", "test"], default="train"
    )

    commands.add_parser("devices", help="List available microphone/input devices")

    readiness = commands.add_parser(
        "readiness", help="Check whether a manifest can be treated as final"
    )
    readiness.add_argument("--manifest", required=True, type=Path)
    readiness.add_argument("--minimum-train", type=int, default=80)
    readiness.add_argument("--minimum-validation", type=int, default=15)
    readiness.add_argument("--minimum-test", type=int, default=20)
    readiness.add_argument(
        "--skip-file-check",
        action="store_true",
        help="Skip path existence checks when source manifests were already verified",
    )
    readiness.add_argument(
        "--report",
        type=Path,
        default=None,
        help="Write the readiness result to a JSON report",
    )

    manifest = commands.add_parser(
        "manifest", help="Merge large CSV manifests while preserving metadata"
    )
    manifest.add_argument(
        "action", choices=["merge", "balance", "windowize", "reserve-test"]
    )
    manifest.add_argument(
        "--input", action="append", required=True, type=Path, dest="inputs"
    )
    manifest.add_argument("--output", required=True, type=Path)
    manifest.add_argument("--max-train-per-label", type=int, default=4_000)
    manifest.add_argument("--max-validation-per-label", type=int, default=1_000)
    manifest.add_argument("--max-test-per-label", type=int, default=1_000)
    manifest.add_argument("--max-segments-per-group", type=int, default=4)
    manifest.add_argument(
        "--balance-datasets",
        action="store_true",
        help="Round-robin source datasets within each label before filling a cap",
    )
    manifest.add_argument("--seed", type=int, default=42)
    manifest.add_argument("--head", default="activity")
    manifest.add_argument("--window-seconds", type=float, default=4.0)
    manifest.add_argument("--hop-seconds", type=float, default=2.0)
    manifest.add_argument(
        "--short-policy",
        choices=["drop", "keep"],
        default="drop",
        help="Drop or preserve target-head rows shorter than the fixed window",
    )
    manifest.add_argument("--test-groups-per-label", type=int, default=100)

    dataset = commands.add_parser(
        "dataset", help="Build training manifests from downloaded datasets"
    )
    dataset.add_argument(
        "action",
        choices=[
            "build-aihub-manifest",
            "build-fsd50k-manifest",
            "build-tau-manifest",
            "build-realised-manifest",
        ],
    )
    dataset.add_argument("--root", required=True, type=Path)
    dataset.add_argument("--output", required=True, type=Path)
    dataset.add_argument(
        "--max-per-label",
        type=int,
        default=500,
        help="Maximum examples per label and split; use 0 for all",
    )
    dataset.add_argument("--seed", type=int, default=42)
    dataset.add_argument(
        "--metadata",
        type=Path,
        default=None,
        help="Optional dataset metadata path (TAU defaults to ROOT/metadata.csv)",
    )
    dataset.add_argument(
        "--reference-manifest",
        type=Path,
        default=None,
        help="Existing FSD50K manifest used to preserve source-group splits",
    )
    dataset.add_argument(
        "--include-other",
        action="store_true",
        help="Include non-target TAU sound classes as OTHER hard negatives",
    )

    archives = commands.add_parser(
        "archives", help="Scan or safely extract AI-Hub source ZIPs"
    )
    archives.add_argument("action", choices=["scan", "verify", "extract"])
    archives.add_argument("--root", required=True, type=Path)
    archives.add_argument("--dataset", default=None)
    archives.add_argument("--archive", action="append", default=[])
    archives.add_argument("--all", action="store_true", dest="all_archives")
    archives.add_argument("--wav-check-limit", type=int, default=100)
    archives.add_argument("--minimum-free-gb", type=float, default=20.0)
    archives.add_argument(
        "--staging-root",
        type=Path,
        default=None,
        help="Temporarily extract on a faster drive before verified transfer",
    )
    archives.add_argument(
        "--continue-on-error",
        action="store_true",
        help="Retain a corrupt ZIP and continue with the remaining archives",
    )
    archives.add_argument(
        "--extractor",
        choices=["python", "bandizip"],
        default="python",
        help="ZIP extraction engine",
    )
    archives.add_argument(
        "--bandizip-path",
        type=Path,
        default=None,
        help="Path to Bandizip bz.exe (normally detected automatically)",
    )
    archives.add_argument("--delete-archive-after-verify", action="store_true")
    archives.add_argument("--confirm-delete-originals", default=None)

    return parser


def main() -> None:
    args = _parser().parse_args()
    if args.command == "train":
        result = train_from_manifest(
            args.manifest,
            args.output,
            cache_path=args.cache,
            check_files=not args.skip_file_check,
            verbose=True,
            batch_size=args.batch_size,
            prefetch_workers=args.prefetch_workers,
            prefetch_batches=args.prefetch_batches,
            benchmark_sample_count=args.benchmark_samples,
            feature_version=args.feature_version,
            tune_classifier=args.tune_classifier,
            classifier_c_values=args.classifier_c_values,
            cost_sensitive_activity=args.cost_sensitive_activity,
            activity_cost_profile=args.activity_cost_profile,
            heads=(
                ("activity", "sound_event")
                if args.head == "both"
                else (args.head,)
            ),
        )
    elif args.command == "benchmark":
        records = list(
            iter_manifest(
                args.manifest, check_files=not args.skip_file_check
            )
        )
        result = benchmark_batch_sizes(
            records,
            YamnetEmbeddingExtractor(feature_version=args.feature_version),
            batch_sizes=args.batch_sizes,
            sample_count=args.samples,
            prefetch_workers=args.prefetch_workers,
        )
        result["manifest"] = str(args.manifest.resolve())
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
    elif args.command == "ensemble-activity":
        if len(args.model) != len(args.weights):
            raise SystemExit("--model count must match --weights count")
        result = build_activity_ensemble(
            args.model,
            args.weights,
            args.output,
            model_version=args.model_version,
        )
    elif args.command == "predict":
        prediction = predict_file(args.wav, args.model)
        result = prediction_to_event(
            prediction,
            trace_id=args.trace_id or f"trace-{uuid4()}",
        )
    elif args.command == "live":
        if args.max_windows is not None and args.max_windows < 1:
            raise ValueError("max-windows must be positive")
        config = FusionConfig(
            activity_confidence=args.activity_threshold,
            enter_quiet_count=args.enter_count,
            exit_quiet_count=args.exit_count,
            heartbeat_seconds=args.heartbeat_seconds,
            context_ttl_seconds=args.context_ttl_seconds,
            sound_confidence=args.sound_threshold,
            sound_confirmation_count=args.sound_confirm_count,
            sound_cooldown_seconds=args.sound_cooldown_seconds,
        )
        predictor = HybridAudioPredictor(args.activity_model, args.sound_model)
        engine = LiveAudioContextEngine(predictor, config=config)
        if args.wav is not None:
            windows = iter_waveform_windows(
                load_wav_16k_mono(args.wav),
                window_seconds=args.window_seconds,
                hop_seconds=args.hop_seconds,
            )
            source = str(args.wav.resolve())
        else:
            device = (
                int(args.device)
                if args.device and args.device.isdigit()
                else args.device
            )
            windows = iter_microphone_windows(
                window_seconds=args.window_seconds,
                hop_seconds=args.hop_seconds,
                device=device,
            )
            source = f"microphone:{device if device is not None else 'default'}"
        trace_id = args.trace_id or f"trace-{uuid4()}"
        replay_base_time = datetime.now(timezone.utc) if args.wav is not None else None
        print(
            f"live source={source} window={args.window_seconds:.1f}s "
            f"hop={args.hop_seconds:.1f}s trace_id={trace_id}",
            file=sys.stderr,
            flush=True,
        )
        try:
            for index, frame in enumerate(
                run_windows(
                    engine,
                    windows,
                    trace_id=trace_id,
                    max_windows=args.max_windows,
                    base_time=replay_base_time,
                    window_seconds=args.window_seconds,
                    hop_seconds=args.hop_seconds,
                ),
                start=1,
            ):
                print(
                    f"window={index} activity={frame.activity.label} "
                    f"({frame.activity.confidence:.3f}) "
                    f"sound={frame.sound_event.label} "
                    f"({frame.sound_event.confidence:.3f}) "
                    f"stable={engine.stabilizer.activity.label}",
                    file=sys.stderr,
                    flush=True,
                )
                if args.feature_events:
                    event = prediction_to_feature_event(
                        frame.activity,
                        trace_id=trace_id,
                        speech_ratio=frame.speech_ratio,
                        duration_seconds=(
                            args.window_seconds + (index - 1) * args.hop_seconds
                        ),
                        now=frame.timestamp,
                        ttl_seconds=max(1, args.context_ttl_seconds),
                        simulated=args.wav is not None,
                    )
                    print(
                        json.dumps(event, ensure_ascii=False, separators=(",", ":")),
                        flush=True,
                    )
                else:
                    for event in frame.events:
                        print(
                            json.dumps(event, ensure_ascii=False, separators=(",", ":")),
                            flush=True,
                        )
        except KeyboardInterrupt:
            print("live inference stopped", file=sys.stderr, flush=True)
        return
    elif args.command == "record":
        print(f"Recording {args.seconds:.1f} seconds...", flush=True)
        device = int(args.device) if args.device and args.device.isdigit() else args.device
        waveform = record_clip(args.seconds, device=device)
        output = save_clip(args.output, waveform)
        append_manifest(
            args.manifest,
            audio_path=output,
            head=args.head,
            label=args.label,
            split=args.split,
        )
        result = {
            "recorded": str(output),
            "seconds": args.seconds,
            "head": args.head,
            "label": args.label.upper(),
            "split": args.split,
        }
    elif args.command == "dataset":
        maximum = None if args.max_per_label == 0 else args.max_per_label
        if args.action == "build-aihub-manifest":
            result = build_aihub_manifest(
                args.root,
                args.output,
                max_per_label=maximum,
                seed=args.seed,
                verbose=True,
            )
        elif args.action == "build-fsd50k-manifest":
            result = build_fsd50k_manifest(
                args.root,
                args.output,
                max_per_label_per_split=maximum,
                seed=args.seed,
            )
        elif args.action == "build-tau-manifest":
            result = build_tau_manifest(
                args.root,
                args.output,
                metadata=args.metadata,
                reference_manifest=args.reference_manifest,
                max_per_label_per_split=maximum,
                include_other=args.include_other,
                seed=args.seed,
            )
        else:
            result = build_realised_manifest(
                args.root,
                args.output,
                max_per_split=maximum,
                seed=args.seed,
            )
    elif args.command == "readiness":
        result = assess_final_readiness(
            args.manifest,
            minimum_train_per_label=args.minimum_train,
            minimum_validation_per_label=args.minimum_validation,
            minimum_test_per_label=args.minimum_test,
            check_files=not args.skip_file_check,
            progress=lambda message: print(message, file=sys.stderr, flush=True),
        )
    elif args.command == "manifest":
        if args.action == "merge":
            result = merge_manifests(args.inputs, args.output)
        elif args.action == "balance":
            if len(args.inputs) != 1:
                raise ValueError("Manifest balance requires exactly one --input")
            result = build_balanced_manifest(
                args.inputs[0],
                args.output,
                max_train_per_label=args.max_train_per_label,
                max_validation_per_label=args.max_validation_per_label,
                max_test_per_label=args.max_test_per_label,
                max_segments_per_group=args.max_segments_per_group,
                balance_datasets=args.balance_datasets,
                seed=args.seed,
            )
        elif args.action == "windowize":
            if len(args.inputs) != 1:
                raise ValueError("Manifest windowize requires exactly one --input")
            result = windowize_manifest(
                args.inputs[0],
                args.output,
                head=args.head,
                window_seconds=args.window_seconds,
                hop_seconds=args.hop_seconds,
                short_policy=args.short_policy,
            )
        else:
            if len(args.inputs) != 1:
                raise ValueError("Manifest reserve-test requires exactly one --input")
            result = reserve_validation_groups(
                args.inputs[0],
                args.output,
                head=args.head,
                groups_per_label=args.test_groups_per_label,
                seed=args.seed,
            )
    elif args.command == "archives":
        if args.action == "scan":
            result = scan_archives(args.root)
        elif args.action == "verify":
            selected = select_archives(
                args.root,
                dataset=args.dataset,
                names=args.archive,
                all_archives=args.all_archives,
            )
            result = verify_selected_archives(selected, root=args.root)
        else:
            selected = select_archives(
                args.root,
                dataset=args.dataset,
                names=args.archive,
                all_archives=args.all_archives,
            )
            result = extract_selected_archives(
                selected,
                root=args.root,
                wav_check_limit=args.wav_check_limit,
                minimum_free_bytes=int(args.minimum_free_gb * 1024**3),
                delete_archive_after_verify=args.delete_archive_after_verify,
                delete_confirmation=args.confirm_delete_originals,
                staging_root=args.staging_root,
                continue_on_archive_error=args.continue_on_error,
                extractor=args.extractor,
                bandizip_path=args.bandizip_path,
            )
            if args.delete_archive_after_verify:
                result["delete_confirmation_phrase"] = DELETE_CONFIRMATION
    else:
        result = {"input_devices": list_input_devices()}
    if args.command == "readiness" and args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
