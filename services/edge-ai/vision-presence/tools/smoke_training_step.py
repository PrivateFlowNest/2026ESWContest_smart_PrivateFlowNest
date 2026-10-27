"""Run one real CUDA forward/backward step on the generated COCO smoke set."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import torch
from yolox.exp import get_exp


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--coco-dir", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--checkpoint", type=Path)
    args = parser.parse_args()
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU is required for the training smoke test")

    os.environ["HOMEMIND_VISION_COCO_DIR"] = str(args.coco_dir.resolve())
    experiment = get_exp(str(args.experiment.resolve()), None)
    experiment.data_num_workers = 0
    loader = experiment.get_data_loader(
        batch_size=args.batch_size,
        is_distributed=False,
        no_aug=True,
        cache_img=None,
    )
    images, targets, _, _ = next(iter(loader))
    device = torch.device("cuda:0")
    model = experiment.get_model().to(device).train()
    torch.cuda.reset_peak_memory_stats(device)
    outputs = model(images.to(device), targets.to(device))
    loss = outputs["total_loss"]
    loss.backward()
    if args.checkpoint is not None:
        args.checkpoint.parent.mkdir(parents=True, exist_ok=True)
        torch.save({"model": model.state_dict(), "smoke_only": True}, args.checkpoint)
    report = {
        "status": "ok",
        "device": torch.cuda.get_device_name(device),
        "batch_size": args.batch_size,
        "input_shape": list(images.shape),
        "total_loss": round(float(loss.detach().cpu()), 6),
        "peak_vram_mib": round(torch.cuda.max_memory_allocated(device) / 1024**2, 1),
        "checkpoint": str(args.checkpoint.resolve()) if args.checkpoint else None,
    }
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
