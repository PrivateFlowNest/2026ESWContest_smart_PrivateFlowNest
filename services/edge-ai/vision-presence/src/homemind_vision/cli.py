"""CLI for HomeMind vision candidate experiments."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .benchmark import benchmark_candidates, write_report
from .dataset import (
    accept_all_proposals,
    assess_readiness,
    assign_splits,
    capture_background,
    capture_frames,
    export_coco,
    init_dataset,
    propose_annotations,
    render_previews,
    review_sample,
    set_background,
)
from .detection import detect_file, save_annotated


def main() -> None:
    parser = argparse.ArgumentParser(prog="homemind-vision")
    commands = parser.add_subparsers(dest="command", required=True)
    benchmark = commands.add_parser("benchmark", help="Benchmark ONNX candidates on CPU")
    benchmark.add_argument("models", nargs="+", type=Path)
    benchmark.add_argument("--runs", type=int, default=100)
    benchmark.add_argument("--warmup-runs", type=int, default=10)
    benchmark.add_argument("--threads", type=int, default=4)
    benchmark.add_argument("--streams", type=int, default=2)
    benchmark.add_argument(
        "--report", type=Path, default=Path("artifacts/pc-candidate-benchmark.json")
    )
    detect = commands.add_parser("detect", help="Run a surrogate-image YOLOX check")
    detect.add_argument("model", type=Path)
    detect.add_argument("image", type=Path)
    detect.add_argument("--output", type=Path, required=True)
    detect.add_argument("--threads", type=int, default=4)
    detect.add_argument("--score", type=float, default=0.25)

    dataset = commands.add_parser("dataset", help="Prepare the local FIGURE dataset")
    dataset_commands = dataset.add_subparsers(dest="dataset_command", required=True)
    dataset_init = dataset_commands.add_parser("init")
    dataset_init.add_argument("--root", type=Path, required=True)

    background = dataset_commands.add_parser("background")
    background.add_argument("--root", type=Path, required=True)
    background.add_argument("--camera-id", required=True)
    background_source = background.add_mutually_exclusive_group(required=True)
    background_source.add_argument("--image", type=Path)
    background_source.add_argument("--source", help="Camera index, such as 0")

    capture = dataset_commands.add_parser("capture")
    capture.add_argument("--root", type=Path, required=True)
    capture.add_argument("--source", required=True, help="Video path or camera index")
    capture.add_argument("--camera-id", required=True)
    capture.add_argument("--source-kind", choices=["FINAL", "SURROGATE"], required=True)
    capture.add_argument("--group-id", required=True)
    capture.add_argument("--every-seconds", type=float, default=1.0)
    capture.add_argument("--max-frames", type=int, default=100)
    capture.add_argument("--lighting", default="UNKNOWN")
    capture.add_argument("--distance-band", default="UNKNOWN")
    capture.add_argument("--occlusion", default="NONE")

    propose = dataset_commands.add_parser("propose")
    propose.add_argument("--root", type=Path, required=True)
    propose.add_argument("--camera-id", default=None)
    propose.add_argument("--threshold", type=int, default=30)
    propose.add_argument("--min-area-ratio", type=float, default=0.001)
    propose.add_argument("--max-area-ratio", type=float, default=0.50)
    propose.add_argument("--margin-pixels", type=int, default=4)

    review = dataset_commands.add_parser("review")
    review.add_argument("--root", type=Path, required=True)
    review.add_argument("--sample-id", required=True)
    review.add_argument("--decision", choices=["ACCEPTED", "EMPTY", "REJECTED"], required=True)
    review.add_argument("--bbox", type=int, nargs=4, default=None, metavar=("X1", "Y1", "X2", "Y2"))

    preview = dataset_commands.add_parser("preview")
    preview.add_argument("--root", type=Path, required=True)

    accept_proposed = dataset_commands.add_parser(
        "accept-proposed", help="Smoke/generated data only; do not use blindly on real captures"
    )
    accept_proposed.add_argument("--root", type=Path, required=True)

    split = dataset_commands.add_parser("split")
    split.add_argument("--root", type=Path, required=True)
    split.add_argument("--seed", default="homemind-vision-v1")

    export = dataset_commands.add_parser("export-coco")
    export.add_argument("--root", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    export.add_argument("--include-surrogate-train", action="store_true")

    ready = dataset_commands.add_parser("ready")
    ready.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "detect":
        detections, image = detect_file(
            args.model,
            args.image,
            threads=args.threads,
            score_threshold=args.score,
        )
        output = save_annotated(image, detections, args.output)
        print(
            json.dumps(
                {
                    "model": str(args.model.resolve()),
                    "image": str(args.image.resolve()),
                    "output": str(output.resolve()),
                    "detections": [
                        {
                            "class_id": item.class_id,
                            "confidence": round(item.confidence, 6),
                            "box": [round(value, 3) for value in item.box],
                        }
                        for item in detections
                    ],
                },
                ensure_ascii=False,
                indent=2,
            )
        )
        return
    if args.command == "dataset":
        if args.dataset_command == "init":
            result = init_dataset(args.root)
        elif args.dataset_command == "background":
            path = (
                set_background(args.root, args.image, camera_id=args.camera_id)
                if args.image is not None
                else capture_background(args.root, args.source, camera_id=args.camera_id)
            )
            result = {"background": str(path)}
        elif args.dataset_command == "capture":
            result = capture_frames(
                args.root,
                args.source,
                camera_id=args.camera_id,
                source_kind=args.source_kind,
                group_id=args.group_id,
                every_seconds=args.every_seconds,
                max_frames=args.max_frames,
                lighting=args.lighting,
                distance_band=args.distance_band,
                occlusion=args.occlusion,
            )
        elif args.dataset_command == "propose":
            result = propose_annotations(
                args.root,
                camera_id=args.camera_id,
                threshold=args.threshold,
                min_area_ratio=args.min_area_ratio,
                max_area_ratio=args.max_area_ratio,
                margin_pixels=args.margin_pixels,
            )
        elif args.dataset_command == "review":
            result = review_sample(
                args.root,
                args.sample_id,
                args.decision,
                bbox_xyxy=args.bbox,
            )
        elif args.dataset_command == "preview":
            result = render_previews(args.root)
        elif args.dataset_command == "accept-proposed":
            result = accept_all_proposals(args.root)
        elif args.dataset_command == "split":
            result = assign_splits(args.root, seed=args.seed)
        elif args.dataset_command == "export-coco":
            result = export_coco(
                args.root,
                args.output,
                include_surrogate_train=args.include_surrogate_train,
            )
        else:
            result = assess_readiness(args.root)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if args.dataset_command == "ready" and not result["ready"]:
            raise SystemExit(2)
        return
    report = benchmark_candidates(
        args.models,
        runs=args.runs,
        warmup_runs=args.warmup_runs,
        threads=args.threads,
        streams=args.streams,
    )
    output = write_report(report, args.report)
    report["report"] = str(output.resolve())
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
