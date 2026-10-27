"""YOLOX raw-output decoding used for PC surrogate checks."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import cv2
import numpy as np


@dataclass(frozen=True)
class Detection:
    class_id: int
    confidence: float
    box: tuple[float, float, float, float]


def preprocess_bgr(image: np.ndarray, input_size: tuple[int, int]) -> tuple[np.ndarray, float]:
    if image.ndim != 3 or image.shape[2] != 3:
        raise ValueError("expected one BGR image")
    height, width = input_size
    ratio = min(height / image.shape[0], width / image.shape[1])
    resized = cv2.resize(
        image,
        (int(image.shape[1] * ratio), int(image.shape[0] * ratio)),
        interpolation=cv2.INTER_LINEAR,
    ).astype(np.float32)
    padded = np.full((height, width, 3), 114.0, dtype=np.float32)
    padded[: resized.shape[0], : resized.shape[1]] = resized
    return np.ascontiguousarray(padded.transpose(2, 0, 1)[None]), ratio


def decode_raw_predictions(raw: np.ndarray, input_size: tuple[int, int]) -> np.ndarray:
    if raw.ndim != 3 or raw.shape[0] != 1 or raw.shape[2] < 6:
        raise ValueError(f"unexpected YOLOX output shape: {raw.shape}")
    height, width = input_size
    grids = []
    expanded_strides = []
    for stride in (8, 16, 32):
        hsize, wsize = height // stride, width // stride
        yv, xv = np.meshgrid(np.arange(hsize), np.arange(wsize), indexing="ij")
        grid = np.stack((xv, yv), axis=2).reshape(1, -1, 2)
        grids.append(grid)
        expanded_strides.append(np.full((*grid.shape[:2], 1), stride))
    grid = np.concatenate(grids, axis=1).astype(np.float32)
    stride_values = np.concatenate(expanded_strides, axis=1).astype(np.float32)
    if grid.shape[1] != raw.shape[1]:
        raise ValueError("model proposal count does not match 416 YOLOX strides")
    decoded = raw.copy()
    decoded[..., :2] = (decoded[..., :2] + grid) * stride_values
    decoded[..., 2:4] = np.exp(decoded[..., 2:4]) * stride_values
    return decoded


def _iou(reference: np.ndarray, boxes: np.ndarray) -> np.ndarray:
    left_top = np.maximum(reference[:2], boxes[:, :2])
    right_bottom = np.minimum(reference[2:], boxes[:, 2:])
    intersection = np.prod(np.maximum(0.0, right_bottom - left_top), axis=1)
    reference_area = max(0.0, (reference[2] - reference[0]) * (reference[3] - reference[1]))
    box_areas = np.maximum(0.0, boxes[:, 2] - boxes[:, 0]) * np.maximum(
        0.0, boxes[:, 3] - boxes[:, 1]
    )
    return intersection / np.maximum(reference_area + box_areas - intersection, 1e-9)


def _nms(boxes: np.ndarray, scores: np.ndarray, threshold: float) -> list[int]:
    order = scores.argsort()[::-1]
    keep: list[int] = []
    while order.size:
        current = int(order[0])
        keep.append(current)
        if order.size == 1:
            break
        remainder = order[1:]
        order = remainder[_iou(boxes[current], boxes[remainder]) <= threshold]
    return keep


def postprocess(
    raw: np.ndarray,
    *,
    input_size: tuple[int, int],
    ratio: float,
    image_shape: tuple[int, int],
    score_threshold: float = 0.25,
    nms_threshold: float = 0.45,
) -> list[Detection]:
    decoded = decode_raw_predictions(raw, input_size)[0]
    centers, sizes = decoded[:, :2], decoded[:, 2:4]
    boxes = np.concatenate((centers - sizes / 2.0, centers + sizes / 2.0), axis=1)
    class_ids = decoded[:, 5:].argmax(axis=1)
    class_scores = decoded[:, 5:].max(axis=1)
    scores = decoded[:, 4] * class_scores
    mask = scores >= score_threshold
    boxes, scores, class_ids = boxes[mask], scores[mask], class_ids[mask]
    if not boxes.size:
        return []
    boxes /= ratio
    image_h, image_w = image_shape
    boxes[:, [0, 2]] = boxes[:, [0, 2]].clip(0, image_w - 1)
    boxes[:, [1, 3]] = boxes[:, [1, 3]].clip(0, image_h - 1)
    detections: list[Detection] = []
    for class_id in np.unique(class_ids):
        indices = np.where(class_ids == class_id)[0]
        for kept in _nms(boxes[indices], scores[indices], nms_threshold):
            index = int(indices[kept])
            detections.append(
                Detection(
                    class_id=int(class_ids[index]),
                    confidence=float(scores[index]),
                    box=tuple(float(value) for value in boxes[index]),
                )
            )
    return sorted(detections, key=lambda item: item.confidence, reverse=True)


def detect_file(
    model_path: str | Path,
    image_path: str | Path,
    *,
    threads: int = 4,
    score_threshold: float = 0.25,
) -> tuple[list[Detection], np.ndarray]:
    import onnxruntime as ort

    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(
        str(Path(model_path).resolve()),
        sess_options=options,
        providers=["CPUExecutionProvider"],
    )
    input_meta = session.get_inputs()[0]
    shape = input_meta.shape
    if len(shape) != 4 or not all(isinstance(value, int) for value in shape):
        raise ValueError(f"fixed NCHW input required, got {shape}")
    image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"could not read image: {image_path}")
    tensor, ratio = preprocess_bgr(image, (shape[2], shape[3]))
    raw = session.run(None, {input_meta.name: tensor})[0]
    detections = postprocess(
        raw,
        input_size=(shape[2], shape[3]),
        ratio=ratio,
        image_shape=image.shape[:2],
        score_threshold=score_threshold,
    )
    return detections, image


def save_annotated(image: np.ndarray, detections: list[Detection], path: str | Path) -> Path:
    output = image.copy()
    for item in detections:
        x1, y1, x2, y2 = (int(round(value)) for value in item.box)
        cv2.rectangle(output, (x1, y1), (x2, y2), (50, 220, 50), 2)
        cv2.putText(
            output,
            f"class={item.class_id} {item.confidence:.2f}",
            (x1, max(15, y1 - 5)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.5,
            (50, 220, 50),
            1,
            cv2.LINE_AA,
        )
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(target), output):
        raise OSError(f"could not write image: {target}")
    return target
