"""Create a clearly marked surrogate dataset to exercise the data pipeline."""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np

from homemind_vision.dataset import (
    accept_all_proposals,
    add_image,
    assign_splits,
    export_coco,
    init_dataset,
    propose_annotations,
    render_previews,
    set_background,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("data/smoke-dataset"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/smoke-coco"))
    args = parser.parse_args()
    init_dataset(args.root)
    manifest = args.root / "annotations.jsonl"
    existing_rows = sum(1 for line in manifest.read_text(encoding="utf-8").splitlines() if line)
    if existing_rows == 0:
        for camera_index, camera_id in enumerate(("CAM-INDOOR-01", "CAM-OUTDOOR-01")):
            background = np.full((240, 320, 3), 25 + camera_index * 10, dtype=np.uint8)
            background_file = args.root / f"{camera_id}-smoke-background.png"
            cv2.imwrite(str(background_file), background)
            set_background(args.root, background_file, camera_id=camera_id)
            background_file.unlink()

            for index in range(6):
                image = background.copy()
                x1 = 25 + index * 35
                y1 = 70 + (index % 2) * 15
                cv2.rectangle(
                    image, (x1, y1), (x1 + 24, y1 + 85), (230, 230, 230), -1
                )
                add_image(
                    args.root,
                    image,
                    camera_id=camera_id,
                    source_kind="SURROGATE",
                    group_id=f"SMOKE-ONLY-{camera_id}",
                    lighting="SYNTHETIC",
                    distance_band="SYNTHETIC",
                )

    proposed = propose_annotations(args.root, threshold=20)
    accepted = accept_all_proposals(args.root)
    previews = render_previews(args.root, states=("ACCEPTED",))
    splits = assign_splits(args.root)
    exported = export_coco(args.root, args.output, include_surrogate_train=True)
    print(
        {
            "warning": "SURROGATE smoke data only; never use as final validation/test",
            "reused_existing_rows": existing_rows,
            "proposed": proposed,
            "accepted": accepted,
            "previews": previews,
            "splits": splits,
            "exported": exported,
        }
    )


if __name__ == "__main__":
    main()
