"""Export the trained one-class YOLOX-Nano checkpoint as fixed 416 ONNX."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import onnx
import torch
from yolox.exp import get_exp


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    experiment = get_exp(str(args.experiment.resolve()), None)
    model = experiment.get_model().cpu().eval()
    checkpoint = torch.load(args.checkpoint.resolve(), map_location="cpu", weights_only=False)
    state_dict = checkpoint["model"] if "model" in checkpoint else checkpoint
    model.load_state_dict(state_dict, strict=True)
    model.head.decode_in_inference = False

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, 3, 416, 416), dtype=torch.float32)
    with torch.inference_mode():
        torch.onnx.export(
            model,
            dummy,
            output,
            input_names=["image"],
            output_names=["predictions"],
            opset_version=13,
            do_constant_folding=True,
            dynamo=False,
        )
    exported = onnx.load(str(output))
    onnx.checker.check_model(exported)
    payload = output.read_bytes()
    report = {
        "model": "YOLOX-Nano",
        "classes": ["FIGURE"],
        "input": [1, 3, 416, 416],
        "output": str(output),
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "checkpoint": str(args.checkpoint.resolve()),
    }
    metadata = output.with_suffix(".json")
    metadata.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
