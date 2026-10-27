import json

import cv2
import numpy as np

from homemind_vision.dataset import (
    accept_all_proposals,
    add_image,
    assess_readiness,
    assign_splits,
    export_coco,
    init_dataset,
    propose_annotations,
    review_sample,
    set_background,
)


def image_with_box(box=None):
    image = np.full((120, 160, 3), 30, dtype=np.uint8)
    if box:
        x1, y1, x2, y2 = box
        cv2.rectangle(image, (x1, y1), (x2, y2), (230, 230, 230), -1)
    return image


def test_init_and_background_difference_propose_review(tmp_path):
    root = tmp_path / "dataset"
    init_dataset(root)
    background_path = tmp_path / "empty.png"
    cv2.imwrite(str(background_path), image_with_box())
    set_background(root, background_path, camera_id="CAM-INDOOR-01")
    row = add_image(
        root,
        image_with_box((40, 30, 70, 90)),
        camera_id="CAM-INDOOR-01",
        source_kind="FINAL",
        group_id="indoor-session-01",
    )

    result = propose_annotations(root, camera_id="CAM-INDOOR-01", threshold=20)
    proposed = json.loads((root / "annotations.jsonl").read_text(encoding="utf-8"))

    assert result == {"proposed": 1}
    assert proposed["bbox_xyxy"][0] <= 40
    assert proposed["bbox_xyxy"][2] >= 70
    reviewed = review_sample(root, row["sample_id"], "ACCEPTED")
    assert reviewed["review_state"] == "ACCEPTED"


def test_surrogate_is_train_only_and_excluded_by_default(tmp_path):
    root = tmp_path / "dataset"
    init_dataset(root)
    final_rows = []
    for index in range(3):
        row = add_image(
            root,
            image_with_box((20, 20, 50, 80)),
            camera_id="CAM-INDOOR-01",
            source_kind="FINAL",
            group_id=f"final-group-{index}",
        )
        final_rows.append(review_sample(root, row["sample_id"], "ACCEPTED", bbox_xyxy=[20, 20, 50, 80]))
    surrogate = add_image(
        root,
        image_with_box((30, 20, 60, 80)),
        camera_id="CAM-OUTDOOR-01",
        source_kind="SURROGATE",
        group_id="surrogate-group",
    )
    review_sample(root, surrogate["sample_id"], "ACCEPTED", bbox_xyxy=[30, 20, 60, 80])

    assign_splits(root)
    rows = [json.loads(line) for line in (root / "annotations.jsonl").read_text(encoding="utf-8").splitlines()]
    final_splits = {row["split"] for row in rows if row["source_kind"] == "FINAL"}
    surrogate_row = next(row for row in rows if row["source_kind"] == "SURROGATE")
    output = tmp_path / "coco"
    report = export_coco(root, output)

    assert final_splits == {"train", "validation", "test"}
    assert surrogate_row["split"] == "train"
    assert sum(item["images"] for item in report["splits"].values()) == 3
    exported = json.loads((output / "annotations" / "instances_train2017.json").read_text(encoding="utf-8"))
    assert all(image["group_id"] != "surrogate-group" for image in exported["images"])


def test_groups_never_leak_and_readiness_reports_missing_final_data(tmp_path):
    root = tmp_path / "dataset"
    init_dataset(root)
    for group_index in range(10):
        for frame_index in range(2):
            row = add_image(
                root,
                image_with_box((10, 10, 40, 70)),
                camera_id="CAM-INDOOR-01" if group_index % 2 else "CAM-OUTDOOR-01",
                source_kind="FINAL",
                group_id=f"session-{group_index}",
            )
            review_sample(root, row["sample_id"], "ACCEPTED", bbox_xyxy=[10, 10, 40, 70])
    assign_splits(root)
    report = assess_readiness(root)

    assert report["group_leakage"] == []
    assert report["independent_groups_by_split"] == {"train": 6, "validation": 2, "test": 2}
    assert report["ready"] is False
    assert any("need 300" in blocker for blocker in report["blockers"])


def test_generated_proposals_can_be_batch_accepted_for_smoke_only(tmp_path):
    root = tmp_path / "dataset"
    init_dataset(root)
    background_path = tmp_path / "empty.png"
    cv2.imwrite(str(background_path), image_with_box())
    set_background(root, background_path, camera_id="CAM-INDOOR-01")
    add_image(
        root,
        image_with_box((30, 30, 60, 90)),
        camera_id="CAM-INDOOR-01",
        source_kind="SURROGATE",
        group_id="generated",
    )
    propose_annotations(root)
    assert accept_all_proposals(root) == {"accepted": 1}
