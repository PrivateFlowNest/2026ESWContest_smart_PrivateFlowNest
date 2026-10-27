"""hm_vision / homemind-vision (C++) vs services/edge-ai/vision-presence (Python reference).

PC x86_64 Docker only (simulation): nothing here touches a camera or a Raspberry Pi.
Run with the vision-presence venv (cv2, numpy, onnxruntime, jsonschema) and the derived build image:

    $env:HM_NATIVE_IMAGE = "homemind-native-dev:24.04"; $env:HM_NATIVE_BUILDER = "vision"
    services/edge-ai/vision-presence/.venv/Scripts/python -m pytest native/tests/test_vision_parity.py -s \
        -p no:cacheprovider --basetemp "$env:TEMP/hmpt/d1"
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import pytest

os.environ.setdefault("HM_NATIVE_IMAGE", "homemind-native-dev:24.04")
os.environ.setdefault("HM_NATIVE_BUILDER", "vision")

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import CONTRACTS, REPO, build, run_shell, validate_topic_message  # noqa: E402

VISION = REPO / "services" / "edge-ai" / "vision-presence"
sys.path.insert(0, str(VISION / "src"))

np = pytest.importorskip("numpy")
cv2 = pytest.importorskip("cv2")
ort = pytest.importorskip("onnxruntime")
from homemind_vision.detection import postprocess, preprocess_bgr  # noqa: E402
from homemind_vision.zones import BoundingBox, ZoneMap, ZoneStabilizer  # noqa: E402

TARGETS = "hm_vision_probe hm_vision_tests homemind-vision"
PROBE = "libs/hm_vision/hm_vision_probe"
SERVICE = "/build/services/homemind-vision/homemind-vision"
SMOKE_MODEL = VISION / "artifacts" / "smoke-yolox-nano-figure.onnx"
COCO_MODEL = VISION / "artifacts" / "candidates" / "yolox_nano_416.onnx"
REGISTRY = json.loads((CONTRACTS / "pfn.v1" / "device-registry.json").read_text(encoding="utf-8"))
ACTION_MAP = json.loads((CONTRACTS / "pfn.v1" / "ai-action-map.json").read_text(encoding="utf-8"))
REPORT: dict[str, Any] = {"scope": f"PC Docker {os.environ['HM_NATIVE_IMAGE']} (simulation, not a Raspberry Pi)"}


def _images() -> list[Path]:
    found = set()
    for sub in ("data", "artifacts"):
        for pattern in ("*.jpg", "*.jpeg", "*.png"):
            found.update((VISION / sub).rglob(pattern))
    return sorted(found)


IMAGES = _images()


def container(path: Path) -> str:
    return "/w/" + path.resolve().relative_to(REPO).as_posix()


def probe(requests: list[dict[str, Any]], out_dir: Path | None = None, timeout: float = 1800,
          builder: str | None = None) -> list[dict[str, Any]]:
    mounts = [f"{out_dir.resolve().as_posix()}:/out"] if out_dir else []
    lines = [json.dumps(item, ensure_ascii=False) for item in requests]
    extra = {"builder": builder} if builder else {}
    result = run_shell(f"/build/{PROBE}", stdin="\n".join(lines) + "\n", extra_mounts=mounts, timeout=timeout,
                       **extra)
    assert result.returncode == 0, result.stderr
    answers = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert len(answers) == len(lines), result.stderr
    return answers


def ok(answer: dict[str, Any]) -> dict[str, Any]:
    assert "error" not in answer, answer["error"]
    return answer


@pytest.fixture(scope="module", autouse=True)
def built() -> None:
    build(target=TARGETS)
    yield
    print("\n[vision parity report]\n" + json.dumps(REPORT, ensure_ascii=False, indent=1))


@pytest.fixture(scope="module")
def sessions() -> dict[str, Any]:
    out = {}
    for model in (SMOKE_MODEL, COCO_MODEL):
        if model.exists():
            options = ort.SessionOptions()
            options.intra_op_num_threads = 4  # detection.detect_file defaults
            options.inter_op_num_threads = 1
            out[model.name] = ort.InferenceSession(str(model), sess_options=options, providers=["CPUExecutionProvider"])
    return out


def test_unit_tests_pass() -> None:
    result = run_shell("/build/libs/hm_vision/hm_vision_tests")
    assert result.returncode == 0 and "PASSED" in result.stdout, result.stdout[-3000:] + result.stderr


def test_sample_images_found() -> None:
    assert len(IMAGES) >= 30, IMAGES
    REPORT["images"] = len(IMAGES)


# ----------------------------------------------------------------------------------------- decoding

def _variants(tmp: Path) -> list[Path]:
    """Extra encodings of real samples: grayscale/progressive/4:4:4 JPEG, EXIF orientation, PNG kinds."""

    from PIL import Image

    base = cv2.imread(str(VISION / "data" / "surrogate" / "dog.jpg"), cv2.IMREAD_COLOR)
    small = cv2.resize(base, (203, 151), interpolation=cv2.INTER_AREA)
    out = []

    def add(name: str) -> Path:
        path = tmp / name
        out.append(path)
        return path

    cv2.imwrite(str(add("gray.jpg")), cv2.cvtColor(small, cv2.COLOR_BGR2GRAY))
    cv2.imwrite(str(add("progressive.jpg")), small, [cv2.IMWRITE_JPEG_PROGRESSIVE, 1, cv2.IMWRITE_JPEG_QUALITY, 60])
    cv2.imwrite(str(add("s444.jpg")), small, [cv2.IMWRITE_JPEG_SAMPLING_FACTOR, cv2.IMWRITE_JPEG_SAMPLING_FACTOR_444])
    cv2.imwrite(str(add("s422.jpg")), small, [cv2.IMWRITE_JPEG_SAMPLING_FACTOR, cv2.IMWRITE_JPEG_SAMPLING_FACTOR_422])
    rgb = Image.fromarray(cv2.cvtColor(small, cv2.COLOR_BGR2RGB))
    for orientation in range(1, 9):
        exif = Image.Exif()
        exif[0x0112] = orientation
        rgb.save(add(f"exif{orientation}.jpg"), quality=90, exif=exif.tobytes())
    cv2.imwrite(str(add("rgb.png")), small)
    cv2.imwrite(str(add("gray.png")), cv2.cvtColor(small, cv2.COLOR_BGR2GRAY))
    cv2.imwrite(str(add("bgra.png")), cv2.cvtColor(small, cv2.COLOR_BGR2BGRA))
    cv2.imwrite(str(add("rgb16.png")), small.astype(np.uint16) * 256 + 77)
    rgb.convert("P", palette=Image.Palette.ADAPTIVE, colors=37).save(add("palette.png"))
    rgb.convert("1").save(add("bilevel.png"))
    rgb.save(add("interlaced.png"), interlace=1)  # PIL ignores interlace; still a plain PNG
    return out


def test_decode_matches_cv2_imread(tmp_path: Path) -> None:
    variants = _variants(tmp_path)
    requests, expected = [], []
    for index, path in enumerate(IMAGES + variants):
        where = container(path) if path in IMAGES else f"/out/{path.name}"
        requests.append({"op": "decode", "path": where, "out": f"/out/decoded-{index}.bgr"})
        expected.append(cv2.imread(str(path), cv2.IMREAD_COLOR))
    answers = probe(requests, tmp_path)
    mismatched = []
    for index, (answer, reference) in enumerate(zip(answers, expected)):
        ok(answer)
        assert (answer["height"], answer["width"]) == reference.shape[:2], requests[index]
        native = np.fromfile(tmp_path / f"decoded-{index}.bgr", dtype=np.uint8).reshape(reference.shape)
        if not np.array_equal(native, reference):
            mismatched.append((requests[index]["path"], int(np.abs(native.astype(int) - reference).max())))
    REPORT["decode"] = {"files": len(requests), "pixel_exact": len(requests) - len(mismatched), "mismatched": mismatched}
    assert not mismatched


def test_resize_matches_cv2(tmp_path: Path) -> None:
    rng = np.random.default_rng(7)
    sizes = [(640, 480), (1280, 720), (1920, 1080), (832, 832), (832, 624), (416, 416), (320, 240), (768, 576),
             (100, 50), (5, 3), (1, 1), (3, 700), (2592, 1944)]
    sizes += [(int(rng.integers(2, 900)), int(rng.integers(2, 900))) for _ in range(25)]
    requests, expected = [], []
    for index, (width, height) in enumerate(sizes):
        image = rng.integers(0, 256, (height, width, 3), dtype=np.uint8)
        image.tofile(tmp_path / f"src-{index}.bgr")
        ratio = min(416 / height, 416 / width)
        target = (max(1, int(width * ratio)), max(1, int(height * ratio)))
        if index % 3 == 2:  # arbitrary targets too, including upscales
            target = (int(rng.integers(1, 1000)), int(rng.integers(1, 1000)))
        requests.append({"op": "resize_raw", "src": f"/out/src-{index}.bgr", "width": width, "height": height,
                         "dst": list(target), "out": f"/out/dst-{index}.bgr"})
        expected.append(cv2.resize(image, target, interpolation=cv2.INTER_LINEAR))
    answers = probe(requests, tmp_path)
    for index, (answer, reference) in enumerate(zip(answers, expected)):
        ok(answer)
        native = np.fromfile(tmp_path / f"dst-{index}.bgr", dtype=np.uint8).reshape(reference.shape)
        assert np.array_equal(native, reference), (requests[index], int(np.abs(native.astype(int) - reference).max()))
    REPORT["resize"] = {"cases": len(requests), "pixel_exact": len(requests)}


def test_yuyv_matches_cv2(tmp_path: Path) -> None:
    rng = np.random.default_rng(11)
    requests, expected = [], []
    for index, (width, height) in enumerate([(640, 480), (320, 240), (2, 1), (1280, 720), (66, 9)]):
        data = rng.integers(0, 256, (height, width, 2), dtype=np.uint8)
        if index == 0:
            data[:, :, 0] = np.arange(width)[None, :] % 256  # sweep Y
        data.tofile(tmp_path / f"yuyv-{index}.raw")
        requests.append({"op": "yuyv_raw", "src": f"/out/yuyv-{index}.raw", "width": width, "height": height,
                         "out": f"/out/yuyv-{index}.bgr"})
        expected.append(cv2.cvtColor(data, cv2.COLOR_YUV2BGR_YUYV))
    answers = probe(requests, tmp_path)
    for index, (answer, reference) in enumerate(zip(answers, expected)):
        ok(answer)
        native = np.fromfile(tmp_path / f"yuyv-{index}.bgr", dtype=np.uint8).reshape(reference.shape)
        assert np.array_equal(native, reference), requests[index]
    REPORT["yuyv"] = {"cases": len(requests), "pixel_exact": len(requests)}


def test_preprocess_tensor_identical(tmp_path: Path) -> None:
    requests = [{"op": "preprocess", "path": container(path), "out": f"/out/tensor-{index}.f32"}
                for index, path in enumerate(IMAGES)]
    answers = probe(requests, tmp_path)
    for index, (path, answer) in enumerate(zip(IMAGES, answers)):
        ok(answer)
        tensor, ratio = preprocess_bgr(cv2.imread(str(path), cv2.IMREAD_COLOR), (416, 416))
        native = np.fromfile(tmp_path / f"tensor-{index}.f32", dtype=np.float32).reshape(tensor.shape)
        assert answer["ratio"] == ratio
        assert np.array_equal(native, tensor), path
    REPORT["preprocess"] = {"images": len(IMAGES), "tensor_exact": len(IMAGES)}


# ---------------------------------------------------------------------------------------- detection

def _compare(python: list, native: list[dict[str, Any]]) -> dict[str, float]:
    assert len(python) == len(native), (len(python), len(native))
    box, score = 0.0, 0.0
    for expected, actual in zip(python, native):
        assert expected.class_id == actual["class_id"]
        box = max(box, float(np.abs(np.asarray(expected.box) - np.asarray(actual["box"])).max()))
        score = max(score, abs(expected.confidence - actual["confidence"]))
    return {"box_px": box, "score": score}


def _ties(raw: np.ndarray, threshold: float) -> int:
    scores = raw[0, :, 4] * raw[0, :, 5:].max(axis=1)
    classes = raw[0, :, 5:].argmax(axis=1)
    keep = scores >= np.float32(threshold)
    pairs = set(zip(classes[keep].tolist(), scores[keep].tolist()))
    return int(keep.sum()) - len(pairs)


@pytest.mark.parametrize("model,thresholds", [(SMOKE_MODEL, (0.25,)), (COCO_MODEL, (0.25, 0.05, 0.01))])
def test_detection_parity(tmp_path: Path, sessions: dict[str, Any], model: Path, thresholds: tuple[float, ...]) -> None:
    if model.name not in sessions:
        pytest.skip(f"{model} missing")
    session = sessions[model.name]
    requests, cases = [], []
    for index, path in enumerate(IMAGES):
        for threshold in thresholds:
            requests.append({"op": "detect", "path": container(path), "model": container(model), "threads": 4,
                             "score_threshold": threshold, "nms_threshold": 0.45,
                             "raw_out": f"/out/raw-{len(cases)}.f32"})
            cases.append((path, threshold))
    answers = probe(requests, tmp_path)
    worst = {"box_px": 0.0, "score": 0.0, "raw": 0.0, "best_score": 0.0}
    detections = candidates = ties = 0
    for case_index, ((path, threshold), answer) in enumerate(zip(cases, answers)):
        ok(answer)
        image = cv2.imread(str(path), cv2.IMREAD_COLOR)
        tensor, ratio = preprocess_bgr(image, (416, 416))
        raw = session.run(None, {session.get_inputs()[0].name: tensor})[0]
        reference = postprocess(raw, input_size=(416, 416), ratio=ratio, image_shape=image.shape[:2],
                                score_threshold=threshold)
        native_raw = np.fromfile(tmp_path / f"raw-{case_index}.f32", dtype=np.float32).reshape(raw.shape)
        diff = _compare(reference, answer["detections"])
        worst["box_px"] = max(worst["box_px"], diff["box_px"])
        worst["score"] = max(worst["score"], diff["score"])
        worst["raw"] = max(worst["raw"], float(np.abs(native_raw - raw).max()))
        scores = raw[0, :, 4] * raw[0, :, 5:].max(axis=1)
        worst["best_score"] = max(worst["best_score"], abs(float(scores.max()) - answer["best_score"]))
        expected_candidates = int((scores >= np.float32(threshold)).sum())
        assert answer["candidates"] == expected_candidates, (path, threshold)
        detections += len(reference)
        candidates += expected_candidates
        ties += _ties(raw, threshold)
    REPORT[f"detect:{model.name}"] = {"cases": len(cases), "thresholds": list(thresholds), "detections": detections,
                                      "candidates": candidates, "candidate_score_ties": ties, "max_diff": worst,
                                      "python_onnxruntime": ort.__version__, "native_onnxruntime": "1.20.1"}
    assert worst["box_px"] <= 1.0
    assert worst["score"] <= 1e-4


def test_postprocess_on_identical_raw(tmp_path: Path, sessions: dict[str, Any]) -> None:
    """Same raw tensor into both decoders: isolates decode/NMS from the ONNX Runtime version."""

    rng = np.random.default_rng(3)
    cases = []
    if COCO_MODEL.name in sessions:
        session = sessions[COCO_MODEL.name]
        for path in IMAGES[:: max(1, len(IMAGES) // 8)]:
            image = cv2.imread(str(path), cv2.IMREAD_COLOR)
            tensor, ratio = preprocess_bgr(image, (416, 416))
            raw = session.run(None, {session.get_inputs()[0].name: tensor})[0]
            for threshold in (0.25, 0.01, 0.001):
                cases.append((raw, ratio, image.shape[:2], threshold, 0.45))
    for index in range(30):  # synthetic heads: clustered, heavily overlapping boxes, 1 or 3 classes
        classes = 1 if index % 2 == 0 else 3
        raw = np.zeros((1, 3549, 5 + classes), dtype=np.float32)
        raw[0, :, :2] = rng.uniform(-1.0, 2.0, (3549, 2))
        raw[0, :, 2:4] = rng.uniform(-0.5, 3.0, (3549, 2))
        raw[0, :, 4] = rng.uniform(0, 1, 3549) ** 3
        raw[0, :, 5:] = rng.uniform(0, 1, (3549, classes))
        shape = [(480, 640), (576, 768), (240, 320), (416, 416), (1080, 1920)][index % 5]
        ratio = min(416 / shape[0], 416 / shape[1])
        cases.append((raw, ratio, shape, [0.25, 0.1, 0.5][index % 3], [0.45, 0.3, 0.7][index % 3]))
    requests = []
    for index, (raw, ratio, shape, threshold, nms) in enumerate(cases):
        raw.tofile(tmp_path / f"case-{index}.f32")
        requests.append({"op": "postprocess_raw", "raw": f"/out/case-{index}.f32", "row_width": raw.shape[2],
                         "ratio": ratio, "image": list(shape), "score_threshold": threshold, "nms_threshold": nms})
    answers = probe(requests, tmp_path)
    worst = {"box_px": 0.0, "score": 0.0}
    survivors = ties = 0
    for (raw, ratio, shape, threshold, nms), answer in zip(cases, answers):
        ok(answer)
        reference = postprocess(raw, input_size=(416, 416), ratio=ratio, image_shape=shape,
                                score_threshold=threshold, nms_threshold=nms)
        diff = _compare(reference, answer["detections"])
        worst = {key: max(worst[key], diff[key]) for key in worst}
        survivors += len(reference)
        ties += _ties(raw, threshold)
    REPORT["postprocess_same_raw"] = {"cases": len(cases), "nms_survivors": survivors, "candidate_score_ties": ties,
                                      "max_diff": worst}
    assert worst["box_px"] <= 1e-3 and worst["score"] == 0.0


# -------------------------------------------------------------------------------------------- zones

def _random_polygon(rng: Any, points: int) -> list[list[float]]:
    center = rng.uniform(0.2, 0.8, 2)
    angles = np.sort(rng.uniform(0, 2 * np.pi, points))
    radii = rng.uniform(0.05, 0.5, points)  # star-shaped, often concave
    xy = np.clip(center + np.stack([np.cos(angles), np.sin(angles)], axis=1) * radii[:, None], 0, 1)
    return [[round(float(x), 3), round(float(y), 3)] for x, y in xy]


def test_zone_locate_parity() -> None:
    rng = np.random.default_rng(5)
    template = json.loads((VISION / "config" / "zones.template.json").read_text(encoding="utf-8"))
    configs = [template]
    names = ["BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR"]
    for _ in range(12):
        chosen = rng.permutation(names)[: int(rng.integers(1, 7))]
        configs.append({"camera_id": "CAM-INDOOR-01",
                        "zones": {str(name): _random_polygon(rng, int(rng.integers(3, 9))) for name in chosen}})
    grid = np.round(np.arange(0, 1.0001, 0.05), 2)
    requests, expected = [], []
    for config in configs:
        zones = ZoneMap(config["camera_id"], config["zones"])
        boxes = []
        for _ in range(300):
            x1, x2 = sorted(rng.uniform(0, 1, 2))
            y1, y2 = sorted(rng.uniform(0, 1, 2))
            boxes.append([float(x1), float(y1), float(x2), float(y2), float(rng.uniform(0, 1))])
        for _ in range(200):  # foot points on polygon edges / grid lines
            x1, x2 = sorted(rng.choice(grid, 2, replace=False))
            y1, y2 = sorted(rng.choice(grid, 2, replace=False))
            boxes.append([float(x1), float(y1), float(x2), float(y2), 0.5])
        boxes += [[0.5, 0.1, 0.4, 0.2, 0.5], [0.1, 0.1, 0.2, 1.2, 0.5], [0.1, 0.1, 0.2, 0.2, 1.5], [0, 0, 0, 0, 0]]
        outcome = []
        for box in boxes:
            try:
                outcome.append(zones.locate(BoundingBox(*box)))
            except ValueError as error:
                outcome.append({"error": str(error)})
        requests.append({"op": "locate", "config": config, "boxes": boxes})
        expected.append(outcome)
    answers = probe(requests)
    total = 0
    for answer, outcome in zip(answers, expected):
        assert ok(answer)["zones"] == outcome
        total += len(outcome)
    REPORT["zones"] = {"configs": len(configs), "boxes": total, "identical": total}


def test_zone_config_errors_match() -> None:
    square = [[0, 0], [1, 0], [1, 1], [0, 1]]
    configs = [
        {"camera_id": "CAM-INDOOR-01", "zones": {"KITCHEN": square}},
        {"camera_id": "CAM-INDOOR-01", "zones": {"BED": [[0, 0], [1, 0]]}},
        {"camera_id": "CAM-INDOOR-01", "zones": {"BED": [[0, 0], [1.5, 0], [1, 1]]}},
        {"camera_id": "", "zones": {"BED": square}},
        {"zones": {"BED": square}},
        {"camera_id": "CAM-INDOOR-01"},
        {"camera_id": "CAM-INDOOR-01", "zones": []},
        {"camera_id": None, "zones": {"BED": square}},  # Python str(None) -> "None" (accepted)
        {"camera_id": "CAM-OUTDOOR-01", "zones": {"OUTDOOR": square, "BED": square}},
    ]
    answers = probe([{"op": "zone_config", "config": config} for config in configs])
    for config, answer in zip(configs, answers):
        try:
            value = config
            if not isinstance(value.get("zones"), dict):
                raise ValueError("zone configuration must contain a zones object")
            zones = ZoneMap(str(value.get("camera_id", "")), value["zones"])
            assert answer == {"camera_id": zones.camera_id, "order": list(zones.zones)}, config
        except ValueError as error:
            assert answer == {"error": str(error)}, (config, answer)


def test_stabilizer_parity() -> None:
    rng = np.random.default_rng(9)
    vocabulary = ["BED", "DESK", None, "UNKNOWN", "", "BATHROOM"]
    requests, expected = [], []
    for confirmation, lost in [(1, 1), (3, 3), (3, 2), (2, 5), (4, 1)]:
        for _ in range(10):
            # runs of the same value make confirmations likely
            sequence = []
            while len(sequence) < 300:
                sequence += [vocabulary[int(rng.integers(0, len(vocabulary)))]] * int(rng.integers(1, 7))
            stable = ZoneStabilizer(confirmation_count=confirmation, lost_count=lost)
            expected.append({"outputs": [stable.update(item) for item in sequence], "current": stable.current})
            requests.append({"op": "stabilizer", "confirmation_count": confirmation, "lost_count": lost,
                             "sequence": sequence})
    answers = probe(requests)
    for answer, reference in zip(answers, expected):
        assert ok(answer) == reference
    REPORT["stabilizer"] = {"sequences": len(requests), "updates": sum(len(r["sequence"]) for r in requests),
                            "identical": True}


# ------------------------------------------------------------------------------------- perception

def _check_registry(document: dict[str, Any]) -> None:
    devices = {device["device_id"]: device for device in REGISTRY["devices"]}
    assert document["zone"] in REGISTRY["zones"]
    device = devices[document["source_device_id"]]
    assert device["controller_node_id"] == document["source_node_id"] == "NODE-RPI5-02"


def test_presence_builders_are_contract_valid() -> None:
    from jsonschema import Draft202012Validator, FormatChecker

    schema = json.loads((CONTRACTS / "rpi5-internal" / "context-feature-observation.schema.json").read_text("utf-8"))
    internal = Draft202012Validator(schema, format_checker=FormatChecker())
    requests = []
    for camera in ("CAM-INDOOR-01", "CAM-OUTDOOR-01"):
        for ai_zone in ["BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR", "UNKNOWN"]:
            requests.append({"op": "presence", "camera_id": camera, "zone": ACTION_MAP["zone_map"][ai_zone],
                             "ai_zone": ai_zone, "present": ai_zone != "UNKNOWN", "figure_count": 25,
                             "confidence": 0.912345, "frame_age_ms": 87, "simulated": True})
    for answer in probe(requests):
        perception = ok(answer)["perception"]
        validate_topic_message("pfn/v1/ai/perception", perception)
        _check_registry(perception)
        assert perception["attributes"]["figure_count"] == 20 and perception["confidence"] == 0.9123
        errors = list(internal.iter_errors(answer["observation"]))
        assert not errors, errors[0].message


def _service(args: str, out_dir: Path | None = None) -> list[tuple[str, dict[str, Any]]]:
    mounts = [f"{out_dir.resolve().as_posix()}:/out"] if out_dir else []
    result = run_shell(f"{SERVICE} {args}", extra_mounts=mounts, timeout=900)
    assert result.returncode == 0, result.stderr[-3000:]
    messages = []
    for line in result.stdout.splitlines():
        topic, _, payload = line.partition(" ")
        messages.append((topic, json.loads(payload)))
    return messages


def test_service_offline_smoke_model() -> None:
    config = "/w/native/services/homemind-vision/config/vision.pc.example.json"
    messages = _service(f"--config {config} --stdout --log-level WARNING")
    assert len(messages) == 2  # one initial state per camera (smoke model: no figure)
    for topic, document in messages:
        assert topic == "pfn/v1/ai/perception"
        validate_topic_message(topic, document)
        _check_registry(document)
        assert document["simulated"] is True and document["attributes"]["present"] is False
        assert document["zone"] == "unknown"
    once = _service(f"--config {config} --stdout --once --log-level WARNING")
    assert {document["source_device_id"] for _, document in once} == {"CAM-INDOOR-01", "CAM-OUTDOOR-01"}


def test_service_publishes_over_mqtt(tmp_path: Path) -> None:
    """hm::Bus path against a Mosquitto inside the build container (its own network namespace)."""

    config = json.loads((REPO / "native" / "services" / "homemind-vision" / "config" / "vision.pc.example.json")
                        .read_text(encoding="utf-8"))
    config["broker"]["port"] = 18830
    (tmp_path / "mqtt.json").write_text(json.dumps(config), encoding="utf-8")
    script = ("mosquitto -p 18830 -d && sleep 0.5 && "
              "(mosquitto_sub -h 127.0.0.1 -p 18830 -t 'pfn/v1/#' -v -C 2 -W 60 > /tmp/sub.txt & echo $! > /tmp/sub.pid) && "
              f"sleep 0.5 && {SERVICE} --config /out/mqtt.json --log-level WARNING && "
              "while kill -0 $(cat /tmp/sub.pid) 2>/dev/null; do sleep 0.2; done; cat /tmp/sub.txt")
    result = run_shell(script, extra_mounts=[f"{tmp_path.resolve().as_posix()}:/out"], timeout=300)
    assert result.returncode == 0, result.stderr[-2000:]
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    assert len(lines) == 2, result.stdout + result.stderr
    for line in lines:
        topic, _, payload = line.partition(" ")
        document = json.loads(payload)
        validate_topic_message(topic, document)
        _check_registry(document)
    REPORT["mqtt"] = {"messages": len(lines), "broker": "mosquitto 2.0.18 in the build container"}


def test_service_zone_matches_python(tmp_path: Path, sessions: dict[str, Any]) -> None:
    """COCO candidate model with 'dog' as the figure class: the published zone must equal Python's
    ZoneMap.locate() on the best box's foot point (mapped to the registry zone)."""

    if COCO_MODEL.name not in sessions:
        pytest.skip("COCO candidate model missing")
    zones_file = REPO / "native" / "services" / "homemind-vision" / "config" / "zones.indoor.example.json"
    config = {
        "simulated": True,
        "vision": {"model_path": container(COCO_MODEL), "model_version": "yolox-nano-416-coco-candidate",
                   "threads": 4, "figure_class_ids": [16, 1, 2],
                   "cameras": [{"camera_id": "CAM-INDOOR-01", "zones": container(zones_file),
                                "source": {"type": "images", "path": container(VISION / "data" / "surrogate")}}]},
    }
    (tmp_path / "vision.json").write_text(json.dumps(config), encoding="utf-8")
    [(topic, message)] = _service("--config /out/vision.json --stdout --once --log-level WARNING", tmp_path)
    validate_topic_message(topic, message)
    _check_registry(message)
    image = cv2.imread(str(VISION / "data" / "surrogate" / "dog.jpg"), cv2.IMREAD_COLOR)
    session = sessions[COCO_MODEL.name]
    tensor, ratio = preprocess_bgr(image, (416, 416))
    raw = session.run(None, {session.get_inputs()[0].name: tensor})[0]
    figures = [d for d in postprocess(raw, input_size=(416, 416), ratio=ratio, image_shape=image.shape[:2])
               if d.class_id in (16, 1, 2)]
    height, width = image.shape[:2]
    best = figures[0]
    box = BoundingBox(best.box[0] / width, best.box[1] / height, best.box[2] / width, best.box[3] / height,
                      best.confidence)
    expected_zone = ACTION_MAP["zone_map"][ZoneMap.load(zones_file).locate(box)]
    assert message["zone"] == expected_zone
    assert message["attributes"]["present"] is True
    assert message["attributes"]["figure_count"] == len(figures)
    assert abs(message["confidence"] - round(best.confidence, 4)) <= 1e-4
    REPORT["service_zone"] = {"zone": message["zone"], "figures": len(figures), "confidence": message["confidence"]}


# ------------------------------------------------------------------------------------------ latency

def test_latency_pc(sessions: dict[str, Any]) -> None:
    release = os.environ["HM_NATIVE_BUILDER"] + "-release"
    build(builder=release, build_type="Release", target="hm_vision_probe")
    results = {}
    for model in (SMOKE_MODEL, COCO_MODEL):
        if model.name not in sessions:
            continue
        for image_path in (VISION / "data" / "surrogate" / "dog.jpg",
                           VISION / "data" / "smoke-dataset" / "backgrounds" / "CAM-INDOOR-01.png"):
            image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
            for threads in (4, 2):
                [native] = probe([{"op": "bench", "path": container(image_path), "model": container(model),
                                   "threads": threads, "runs": 200, "warmup": 20}], builder=release)
                options = ort.SessionOptions()
                options.intra_op_num_threads = threads
                options.inter_op_num_threads = 1
                session = ort.InferenceSession(str(model), sess_options=options, providers=["CPUExecutionProvider"])
                name = session.get_inputs()[0].name
                timings = []
                for run in range(220):
                    started = time.perf_counter()
                    tensor, ratio = preprocess_bgr(image, (416, 416))
                    raw = session.run(None, {name: tensor})[0]
                    postprocess(raw, input_size=(416, 416), ratio=ratio, image_shape=image.shape[:2])
                    if run >= 20:
                        timings.append((time.perf_counter() - started) * 1000)
                key = f"{model.stem}|{image_path.name}|{image.shape[1]}x{image.shape[0]}|threads={threads}"
                results[key] = {
                    "native_total_p50_ms": round(ok(native)["total"]["p50_ms"], 3),
                    "native_total_p95_ms": round(native["total"]["p95_ms"], 3),
                    "native_inference_p50_ms": round(native["inference"]["p50_ms"], 3),
                    "native_preprocess_p50_ms": round(native["preprocess"]["p50_ms"], 3),
                    "python_total_p50_ms": round(float(np.percentile(timings, 50)), 3),
                    "python_total_p95_ms": round(float(np.percentile(timings, 95)), 3),
                }
                assert native["total"]["p95_ms"] < 1000
    REPORT["latency"] = {"label": "PC x86_64 (Ryzen 9 5900X); native = Docker Ubuntu 24.04 Release build, "
                                  "python = Windows venv. Not a Raspberry Pi measurement.", "results": results}
