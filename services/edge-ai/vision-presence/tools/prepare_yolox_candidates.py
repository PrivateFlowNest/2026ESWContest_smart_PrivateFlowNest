"""Download official YOLOX weights through torch.hub and export fixed 416 ONNX."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path

import torch


VARIANTS = {"nano": "yolox_nano", "tiny": "yolox_tiny"}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/candidates"))
    parser.add_argument("--torch-home", type=Path, default=Path(".cache/torch"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.torch_home.mkdir(parents=True, exist_ok=True)
    os.environ["TORCH_HOME"] = str(args.torch_home.resolve())
    manifest = {"source": "Megvii-BaseDetection/YOLOX", "input_size": [1, 3, 416, 416], "models": {}}
    dummy = torch.zeros((1, 3, 416, 416), dtype=torch.float32)

    for label, entrypoint in VARIANTS.items():
        print(f"Preparing YOLOX-{label}...", flush=True)
        model = torch.hub.load(
            "Megvii-BaseDetection/YOLOX",
            entrypoint,
            pretrained=True,
            trust_repo=True,
        )
        model.eval().cpu()
        model.head.decode_in_inference = False
        target = args.output_dir / f"yolox_{label}_416.onnx"
        with torch.inference_mode():
            torch.onnx.export(
                model,
                dummy,
                target,
                input_names=["image"],
                output_names=["predictions"],
                opset_version=13,
                do_constant_folding=True,
                dynamo=False,
            )
        payload = target.read_bytes()
        manifest["models"][label] = {
            "path": str(target.resolve()),
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "parameters": sum(parameter.numel() for parameter in model.parameters()),
            "output": "raw YOLOX predictions; final deployment postprocessing is added after one-class training",
        }
        del model

    manifest_path = args.output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
