"""Local-only capture, annotation proposal, split, and COCO export pipeline."""

from __future__ import annotations

from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import shutil
from typing import Any, Iterable
from uuid import uuid4

import cv2
import numpy as np


CAMERA_IDS = frozenset({"CAM-INDOOR-01", "CAM-OUTDOOR-01"})
SOURCE_KINDS = frozenset({"FINAL", "SURROGATE"})
REVIEW_STATES = frozenset(
    {"UNLABELED", "PROPOSED", "PROPOSED_EMPTY", "ACCEPTED", "EMPTY", "REJECTED"}
)
SPLITS = ("train", "validation", "test")


def _manifest_path(root: Path) -> Path:
    return root / "annotations.jsonl"


def _read_rows(root: str | Path) -> list[dict[str, Any]]:
    manifest = _manifest_path(Path(root))
    if not manifest.exists():
        return []
    rows: list[dict[str, Any]] = []
    for line_number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        value = json.loads(line)
        if not isinstance(value, dict):
            raise ValueError(f"manifest line {line_number} is not an object")
        rows.append(value)
    return rows


def _write_rows(root: Path, rows: Iterable[dict[str, Any]]) -> None:
    target = _manifest_path(root)
    temporary = target.with_suffix(".jsonl.tmp")
    temporary.write_text(
        "".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows),
        encoding="utf-8",
    )
    temporary.replace(target)


def init_dataset(root: str | Path) -> dict[str, Any]:
    target = Path(root).resolve()
    for child in (
        "images/final/CAM-INDOOR-01",
        "images/final/CAM-OUTDOOR-01",
        "images/surrogate/CAM-INDOOR-01",
        "images/surrogate/CAM-OUTDOOR-01",
        "backgrounds",
        "previews",
        "exports",
    ):
        (target / child).mkdir(parents=True, exist_ok=True)
    manifest = _manifest_path(target)
    if not manifest.exists():
        manifest.write_text("", encoding="utf-8")
    descriptor = target / "dataset.json"
    if not descriptor.exists():
        descriptor.write_text(
            json.dumps(
                {
                    "schema_version": "1.0",
                    "created_at": datetime.now(timezone.utc).isoformat(),
                    "privacy": "LOCAL_ONLY",
                    "class_names": ["FIGURE"],
                    "camera_ids": sorted(CAMERA_IDS),
                },
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    return {"root": str(target), "manifest": str(manifest)}


def _safe_relative_image(root: Path, relative: str) -> Path:
    candidate = (root / relative).resolve()
    if root.resolve() not in candidate.parents:
        raise ValueError("image path escapes dataset root")
    return candidate


def add_image(
    root: str | Path,
    image: np.ndarray,
    *,
    camera_id: str,
    source_kind: str,
    group_id: str,
    lighting: str = "UNKNOWN",
    distance_band: str = "UNKNOWN",
    occlusion: str = "NONE",
) -> dict[str, Any]:
    target = Path(root).resolve()
    init_dataset(target)
    if camera_id not in CAMERA_IDS:
        raise ValueError(f"unsupported camera_id: {camera_id}")
    if source_kind not in SOURCE_KINDS:
        raise ValueError(f"unsupported source_kind: {source_kind}")
    if not group_id.strip():
        raise ValueError("group_id is required")
    if image.ndim != 3 or image.shape[2] != 3:
        raise ValueError("expected one BGR image")
    sample_id = f"vision-{uuid4()}"
    relative = Path("images") / source_kind.lower() / camera_id / f"{sample_id}.jpg"
    output = target / relative
    if not cv2.imwrite(str(output), image, [cv2.IMWRITE_JPEG_QUALITY, 95]):
        raise OSError(f"could not write {output}")
    row = {
        "schema_version": "1.0",
        "sample_id": sample_id,
        "group_id": group_id,
        "camera_id": camera_id,
        "source_kind": source_kind,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "image_path": relative.as_posix(),
        "width": int(image.shape[1]),
        "height": int(image.shape[0]),
        "lighting": lighting,
        "distance_band": distance_band,
        "occlusion": occlusion,
        "review_state": "UNLABELED",
        "proposal_method": None,
        "bbox_xyxy": None,
        "split": None,
    }
    rows = _read_rows(target)
    rows.append(row)
    _write_rows(target, rows)
    return row


def capture_frames(
    root: str | Path,
    source: str,
    *,
    camera_id: str,
    source_kind: str,
    group_id: str,
    every_seconds: float = 1.0,
    max_frames: int = 100,
    lighting: str = "UNKNOWN",
    distance_band: str = "UNKNOWN",
    occlusion: str = "NONE",
) -> dict[str, Any]:
    if every_seconds <= 0 or max_frames < 1:
        raise ValueError("every_seconds and max_frames must be positive")
    capture_source: int | str = int(source) if source.isdigit() and not Path(source).exists() else source
    capture = cv2.VideoCapture(capture_source)
    if not capture.isOpened():
        raise ValueError(f"could not open video source: {source}")
    source_fps = capture.get(cv2.CAP_PROP_FPS)
    source_fps = source_fps if source_fps and source_fps > 0 else 30.0
    frame_interval = max(1, int(round(source_fps * every_seconds)))
    saved: list[str] = []
    frame_index = 0
    try:
        while len(saved) < max_frames:
            ok, frame = capture.read()
            if not ok:
                break
            if frame_index % frame_interval == 0:
                row = add_image(
                    root,
                    frame,
                    camera_id=camera_id,
                    source_kind=source_kind,
                    group_id=group_id,
                    lighting=lighting,
                    distance_band=distance_band,
                    occlusion=occlusion,
                )
                saved.append(row["sample_id"])
            frame_index += 1
    finally:
        capture.release()
    return {"captured": len(saved), "sample_ids": saved}


def set_background(root: str | Path, image_path: str | Path, *, camera_id: str) -> Path:
    target = Path(root).resolve()
    init_dataset(target)
    if camera_id not in CAMERA_IDS:
        raise ValueError(f"unsupported camera_id: {camera_id}")
    image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"could not read background: {image_path}")
    output = target / "backgrounds" / f"{camera_id}.png"
    if not cv2.imwrite(str(output), image):
        raise OSError(f"could not write {output}")
    return output


def capture_background(root: str | Path, source: str, *, camera_id: str) -> Path:
    """Capture one empty-room frame from a camera index or video source."""

    capture_source: int | str = (
        int(source) if source.isdigit() and not Path(source).exists() else source
    )
    capture = cv2.VideoCapture(capture_source)
    if not capture.isOpened():
        raise ValueError(f"could not open video source: {source}")
    try:
        ok, frame = capture.read()
    finally:
        capture.release()
    if not ok or frame is None:
        raise ValueError(f"could not capture a background frame: {source}")
    target = Path(root).resolve()
    init_dataset(target)
    temporary = target / "backgrounds" / f".{camera_id}-capture.png"
    if not cv2.imwrite(str(temporary), frame):
        raise OSError(f"could not write {temporary}")
    try:
        return set_background(target, temporary, camera_id=camera_id)
    finally:
        temporary.unlink(missing_ok=True)


def _proposed_bbox(
    image: np.ndarray,
    background: np.ndarray,
    *,
    threshold: int,
    min_area_ratio: float,
    max_area_ratio: float,
    margin_pixels: int,
) -> list[int] | None:
    if image.shape != background.shape:
        background = cv2.resize(background, (image.shape[1], image.shape[0]))
    difference = cv2.absdiff(image, background)
    gray = cv2.cvtColor(difference, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (5, 5), 0)
    mask = cv2.threshold(gray, threshold, 255, cv2.THRESH_BINARY)[1]
    kernel = np.ones((5, 5), dtype=np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=2)
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    image_area = image.shape[0] * image.shape[1]
    accepted = [
        contour
        for contour in contours
        if cv2.contourArea(contour) / image_area >= min_area_ratio
    ]
    if not accepted:
        return None
    points = np.concatenate(accepted, axis=0)
    x, y, width, height = cv2.boundingRect(points)
    ratio = width * height / image_area
    if ratio > max_area_ratio:
        return None
    x1 = max(0, x - margin_pixels)
    y1 = max(0, y - margin_pixels)
    x2 = min(image.shape[1], x + width + margin_pixels)
    y2 = min(image.shape[0], y + height + margin_pixels)
    return [x1, y1, x2, y2]


def propose_annotations(
    root: str | Path,
    *,
    camera_id: str | None = None,
    threshold: int = 30,
    min_area_ratio: float = 0.001,
    max_area_ratio: float = 0.50,
    margin_pixels: int = 4,
) -> dict[str, Any]:
    target = Path(root).resolve()
    if not 0 <= threshold <= 255:
        raise ValueError("threshold must be between 0 and 255")
    if not 0 < min_area_ratio < max_area_ratio <= 1:
        raise ValueError("area ratios are invalid")
    rows = _read_rows(target)
    counts = Counter()
    backgrounds: dict[str, np.ndarray] = {}
    for row in rows:
        if row.get("review_state") != "UNLABELED":
            continue
        if camera_id and row.get("camera_id") != camera_id:
            continue
        active_camera = row["camera_id"]
        if active_camera not in backgrounds:
            background = cv2.imread(
                str(target / "backgrounds" / f"{active_camera}.png"), cv2.IMREAD_COLOR
            )
            if background is None:
                raise ValueError(f"missing background for {active_camera}")
            backgrounds[active_camera] = background
        image = cv2.imread(
            str(_safe_relative_image(target, row["image_path"])), cv2.IMREAD_COLOR
        )
        if image is None:
            counts["missing_image"] += 1
            continue
        bbox = _proposed_bbox(
            image,
            backgrounds[active_camera],
            threshold=threshold,
            min_area_ratio=min_area_ratio,
            max_area_ratio=max_area_ratio,
            margin_pixels=margin_pixels,
        )
        row["bbox_xyxy"] = bbox
        row["review_state"] = "PROPOSED" if bbox else "PROPOSED_EMPTY"
        row["proposal_method"] = "BACKGROUND_DIFFERENCE_V1"
        counts[row["review_state"].lower()] += 1
    _write_rows(target, rows)
    return dict(counts)


def render_previews(
    root: str | Path,
    *,
    states: tuple[str, ...] = ("PROPOSED", "ACCEPTED"),
) -> dict[str, Any]:
    target = Path(root).resolve()
    rows = _read_rows(target)
    written = 0
    for row in rows:
        if row.get("review_state") not in states:
            continue
        image = cv2.imread(
            str(_safe_relative_image(target, row["image_path"])), cv2.IMREAD_COLOR
        )
        if image is None:
            continue
        bbox = row.get("bbox_xyxy")
        if bbox:
            x1, y1, x2, y2 = bbox
            color = (0, 220, 255) if row["review_state"] == "PROPOSED" else (0, 220, 0)
            cv2.rectangle(image, (x1, y1), (x2, y2), color, 2)
            cv2.putText(
                image,
                f"FIGURE {row['review_state']}",
                (x1, max(16, y1 - 6)),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                color,
                1,
                cv2.LINE_AA,
            )
        output = target / "previews" / f"{row['sample_id']}.jpg"
        if cv2.imwrite(str(output), image):
            written += 1
    return {"written": written, "directory": str(target / "previews")}


def review_sample(
    root: str | Path,
    sample_id: str,
    decision: str,
    *,
    bbox_xyxy: list[int] | None = None,
) -> dict[str, Any]:
    decision = decision.upper()
    if decision not in {"ACCEPTED", "EMPTY", "REJECTED"}:
        raise ValueError("decision must be ACCEPTED, EMPTY, or REJECTED")
    target = Path(root).resolve()
    rows = _read_rows(target)
    matches = [row for row in rows if row.get("sample_id") == sample_id]
    if len(matches) != 1:
        raise ValueError(f"sample_id must match exactly one row: {sample_id}")
    row = matches[0]
    if bbox_xyxy is not None:
        if len(bbox_xyxy) != 4:
            raise ValueError("bbox requires four integers")
        x1, y1, x2, y2 = (int(value) for value in bbox_xyxy)
        if not (0 <= x1 < x2 <= row["width"] and 0 <= y1 < y2 <= row["height"]):
            raise ValueError("bbox is outside the image")
        row["bbox_xyxy"] = [x1, y1, x2, y2]
        row["proposal_method"] = "MANUAL"
    if decision == "ACCEPTED" and not row.get("bbox_xyxy"):
        raise ValueError("ACCEPTED requires a bounding box")
    if decision in {"EMPTY", "REJECTED"}:
        row["bbox_xyxy"] = None
    row["review_state"] = decision
    _write_rows(target, rows)
    return row


def accept_all_proposals(root: str | Path) -> dict[str, int]:
    """Convenience for generated smoke data; real captures should be reviewed visually."""

    target = Path(root).resolve()
    rows = _read_rows(target)
    counts = Counter()
    for row in rows:
        if row.get("review_state") == "PROPOSED":
            row["review_state"] = "ACCEPTED"
            counts["accepted"] += 1
        elif row.get("review_state") == "PROPOSED_EMPTY":
            row["review_state"] = "EMPTY"
            counts["empty"] += 1
    _write_rows(target, rows)
    return dict(counts)


def assign_splits(
    root: str | Path,
    *,
    seed: str = "homemind-vision-v1",
) -> dict[str, Any]:
    target = Path(root).resolve()
    rows = _read_rows(target)
    final_groups = sorted(
        {row["group_id"] for row in rows if row.get("source_kind") == "FINAL"},
        key=lambda group: hashlib.sha256(f"{seed}:{group}".encode()).hexdigest(),
    )
    assignments: dict[str, str] = {}
    count = len(final_groups)
    if count >= 3:
        minimum_holdout = 2 if count >= 10 else 1
        validation_count = max(minimum_holdout, round(count * 0.15))
        test_count = max(minimum_holdout, round(count * 0.15))
        while validation_count + test_count >= count:
            if validation_count >= test_count and validation_count > 1:
                validation_count -= 1
            elif test_count > 1:
                test_count -= 1
            else:
                break
        train_end = count - validation_count - test_count
        validation_end = count - test_count
    else:
        train_end = count
        validation_end = count
    for index, group in enumerate(final_groups):
        assignments[group] = (
            "train" if index < train_end else "validation" if index < validation_end else "test"
        )
    for row in rows:
        row["split"] = "train" if row.get("source_kind") == "SURROGATE" else assignments.get(row["group_id"])
    _write_rows(target, rows)
    return {
        "groups": len(assignments),
        "group_counts": dict(Counter(assignments.values())),
        "surrogate_policy": "train_only",
    }


def _copy_image(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def export_coco(
    root: str | Path,
    output: str | Path,
    *,
    include_surrogate_train: bool = False,
) -> dict[str, Any]:
    dataset_root = Path(root).resolve()
    output_root = Path(output).resolve()
    rows = _read_rows(dataset_root)
    if any(row.get("split") not in SPLITS for row in rows if row.get("review_state") in {"ACCEPTED", "EMPTY"}):
        raise ValueError("assign splits before export")
    results: dict[str, Any] = {}
    for split in SPLITS:
        coco_split = "val" if split == "validation" else split
        eligible = []
        for row in rows:
            if row.get("split") != split or row.get("review_state") not in {"ACCEPTED", "EMPTY"}:
                continue
            if row.get("source_kind") == "SURROGATE" and not (
                include_surrogate_train and split == "train"
            ):
                continue
            eligible.append(row)
        images = []
        annotations = []
        for image_id, row in enumerate(eligible, 1):
            extension = Path(row["image_path"]).suffix.lower() or ".jpg"
            filename = f"{row['sample_id']}{extension}"
            _copy_image(
                _safe_relative_image(dataset_root, row["image_path"]),
                output_root / f"{coco_split}2017" / filename,
            )
            images.append(
                {
                    "id": image_id,
                    "file_name": filename,
                    "width": row["width"],
                    "height": row["height"],
                    "camera_id": row["camera_id"],
                    "group_id": row["group_id"],
                }
            )
            bbox = row.get("bbox_xyxy")
            if row["review_state"] == "ACCEPTED" and bbox:
                x1, y1, x2, y2 = bbox
                width, height = x2 - x1, y2 - y1
                annotations.append(
                    {
                        "id": len(annotations) + 1,
                        "image_id": image_id,
                        "category_id": 1,
                        "bbox": [x1, y1, width, height],
                        "area": width * height,
                        "iscrowd": 0,
                    }
                )
        payload = {
            "info": {"description": "HomeMind local FIGURE presence dataset", "version": "1.0"},
            "licenses": [],
            "images": images,
            "annotations": annotations,
            "categories": [{"id": 1, "name": "FIGURE", "supercategory": "presence"}],
        }
        annotation_path = output_root / "annotations" / f"instances_{coco_split}2017.json"
        annotation_path.parent.mkdir(parents=True, exist_ok=True)
        annotation_path.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        results[split] = {"images": len(images), "boxes": len(annotations)}
    return {"output": str(output_root), "splits": results}


def assess_readiness(root: str | Path) -> dict[str, Any]:
    target = Path(root).resolve()
    rows = _read_rows(target)
    final_reviewed = [
        row for row in rows if row.get("source_kind") == "FINAL" and row.get("review_state") in {"ACCEPTED", "EMPTY"}
    ]
    counts = Counter(row.get("split") for row in final_reviewed)
    camera_counts = {
        camera: dict(Counter(row.get("split") for row in final_reviewed if row.get("camera_id") == camera))
        for camera in sorted(CAMERA_IDS)
    }
    groups = {split: {row["group_id"] for row in final_reviewed if row.get("split") == split} for split in SPLITS}
    leakage = sorted((groups["train"] & groups["validation"]) | (groups["train"] & groups["test"]) | (groups["validation"] & groups["test"]))
    blockers = []
    integrity_issues: list[str] = []
    sample_ids = [row.get("sample_id") for row in rows]
    duplicate_ids = sorted(
        sample_id
        for sample_id, count in Counter(sample_ids).items()
        if sample_id and count > 1
    )
    if duplicate_ids:
        integrity_issues.append(f"duplicate sample ids: {duplicate_ids[:5]}")
    for row in final_reviewed:
        sample_id = row.get("sample_id", "<missing>")
        try:
            image_path = _safe_relative_image(target, row["image_path"])
        except (KeyError, ValueError) as error:
            integrity_issues.append(f"{sample_id}: invalid image path ({error})")
            continue
        if not image_path.is_file():
            integrity_issues.append(f"{sample_id}: image file is missing")
        bbox = row.get("bbox_xyxy")
        if row.get("review_state") == "ACCEPTED":
            if not isinstance(bbox, list) or len(bbox) != 4:
                integrity_issues.append(f"{sample_id}: accepted row has no valid bbox")
            else:
                x1, y1, x2, y2 = bbox
                if not (
                    0 <= x1 < x2 <= row.get("width", 0)
                    and 0 <= y1 < y2 <= row.get("height", 0)
                ):
                    integrity_issues.append(f"{sample_id}: bbox is outside the image")
    minimum_images = {"train": 300, "validation": 75, "test": 75}
    minimum_groups = {"train": 6, "validation": 2, "test": 2}
    minimum_camera_images = {"train": 150, "validation": 35, "test": 35}
    minimum_camera_groups = {"train": 3, "validation": 1, "test": 1}
    for split in SPLITS:
        if counts.get(split, 0) < minimum_images[split]:
            blockers.append(f"{split}: need {minimum_images[split]} final reviewed images")
        if len(groups[split]) < minimum_groups[split]:
            blockers.append(f"{split}: need {minimum_groups[split]} independent groups")
        split_rows = [row for row in final_reviewed if row.get("split") == split]
        positives = sum(row["review_state"] == "ACCEPTED" for row in split_rows)
        negatives = sum(row["review_state"] == "EMPTY" for row in split_rows)
        minimum_negatives = max(1, math.ceil(len(split_rows) * 0.10))
        minimum_positives = max(1, math.ceil(len(split_rows) * 0.60))
        if negatives < minimum_negatives:
            blockers.append(
                f"{split}: need at least 10% reviewed EMPTY negative images"
            )
        if positives < minimum_positives:
            blockers.append(
                f"{split}: need at least 60% reviewed FIGURE-positive images"
            )
    for camera in sorted(CAMERA_IDS):
        for split in SPLITS:
            camera_images = camera_counts[camera].get(split, 0)
            if camera_images < minimum_camera_images[split]:
                blockers.append(
                    f"{camera}/{split}: need {minimum_camera_images[split]} "
                    "final reviewed images"
                )
            camera_groups = {
                row["group_id"]
                for row in final_reviewed
                if row.get("camera_id") == camera and row.get("split") == split
            }
            if len(camera_groups) < minimum_camera_groups[split]:
                blockers.append(
                    f"{camera}/{split}: need {minimum_camera_groups[split]} "
                    "independent groups"
                )
    final_pending = Counter(
        row.get("review_state")
        for row in rows
        if row.get("source_kind") == "FINAL"
        and row.get("review_state") in {"UNLABELED", "PROPOSED", "PROPOSED_EMPTY"}
    )
    if final_pending:
        blockers.append(f"final captures still need review: {dict(final_pending)}")
    if leakage:
        blockers.append("group leakage across splits")
    if integrity_issues:
        blockers.append(f"data integrity errors: {len(integrity_issues)}")
    pending = Counter(row.get("review_state") for row in rows)
    return {
        "ready": not blockers,
        "final_reviewed_by_split": dict(counts),
        "final_reviewed_by_camera": camera_counts,
        "independent_groups_by_split": {key: len(value) for key, value in groups.items()},
        "review_states": dict(pending),
        "final_pending_review": dict(final_pending),
        "group_leakage": leakage,
        "integrity_issues": integrity_issues[:20],
        "blockers": blockers,
    }
