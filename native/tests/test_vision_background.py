"""homemind-vision with detector "background" on a synthetic one-axis doll scene (PC x86_64 Docker, simulation).

No trained model: the empty scene is captured with --capture-background, then a figure travels along the rail
ENTRANCE -> BED -> BATHROOM and leaves. The scene has a two-level floor, room light changing by +-10 % per frame,
sensor noise, and a status LED inside the BED zone that is off in the empty scene and lit during the run.
Checks: published zones in order, one figure (the LED is in an `ignore` area), present=false after it leaves,
every message schema-valid; without the ignore area the LED alone is reported as a figure; a missing empty-scene
image stops the service with the command to run.

    services/runtime/.venv/Scripts/python -m pytest native/tests/test_vision_background.py -q
"""

from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hmnative import build, run_shell, validate_topic_message  # noqa: E402

np = pytest.importorskip("numpy")

SERVICE = "/build/services/homemind-vision/homemind-vision"
WIDTH, HEIGHT = 640, 480
ZONES = {"ENTRANCE": [[0.0, 0.55], [0.33, 0.55], [0.33, 1.0], [0.0, 1.0]],
         "BED": [[0.33, 0.55], [0.66, 0.55], [0.66, 1.0], [0.33, 1.0]],
         "BATHROOM": [[0.66, 0.55], [1.0, 0.55], [1.0, 1.0], [0.66, 1.0]]}
LED = (slice(280, 312), slice(296, 328))  # rows, columns: a lit LED with its glow, inside the BED zone
LED_IGNORE = [[0.455, 0.57], [0.52, 0.57], [0.52, 0.66], [0.455, 0.66]]
# figure centre x per frame (None = out of view): wait, entrance, travel, bed, travel, bathroom, leave
ROUTE = [None] * 4 + [105] * 6 + [160, 210, 260] + [320] * 6 + [380, 430, 480] + [530] * 6 + [None] * 6


def write_png(path: Path, bgr: np.ndarray) -> None:
    """8-bit RGB PNG with the standard library (the runtime venv has no OpenCV)."""
    rgb = np.ascontiguousarray(bgr[:, :, ::-1])
    raw = b"".join(b"\x00" + rgb[row].tobytes() for row in range(rgb.shape[0]))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", rgb.shape[1], rgb.shape[0], 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 6))
                     + chunk(b"IEND", b""))


def room() -> np.ndarray:
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    floor = 110 + 40 * (((x // 16) + (y // 16)) % 2) + (x * 20 // WIDTH)
    image = np.stack([floor, floor + 8, floor - 8], axis=-1).astype(np.float32)
    image[40:200, 60:250] = (70, 110, 150)     # furniture
    image[60:180, 400:600] = (150, 140, 90)
    image[LED] = (40, 40, 120)                 # LED off
    return image


def frame(index: int, figure_x: int | None, led_on: bool, rng: np.random.Generator) -> np.ndarray:
    image = room()
    if led_on:
        image[LED] = (60, 60, 255)
    if figure_x is not None:  # roughly made doll: body, head, a darker skirt
        image[360:430, figure_x - 15:figure_x + 15] = (205, 200, 230)
        y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
        image[(x - figure_x) ** 2 + (y - 345) ** 2 <= 14 ** 2] = (190, 210, 235)
        image[405:430, figure_x - 18:figure_x + 18] = (120, 60, 200)
    image *= rng.uniform(0.9, 1.1)
    image += rng.normal(0.0, 3.0, image.shape)
    return np.clip(np.rint(image), 0, 255).astype(np.uint8)


def write_scene(work: Path) -> None:
    rng = np.random.default_rng(20261022 + 7)
    (work / "empty").mkdir()
    (work / "route").mkdir()
    for index in range(10):
        write_png(work / "empty" / f"empty-{index:03d}.png", frame(index, None, False, rng))
    for index, figure_x in enumerate(ROUTE):
        write_png(work / "route" / f"route-{index:03d}.png", frame(index, figure_x, True, rng))
    (work / "zones.json").write_text(json.dumps({"camera_id": "CAM-INDOOR-01", "zones": ZONES}), "utf-8")


def config(work: Path, *, ignore: bool, name: str) -> str:
    camera = {"camera_id": "CAM-INDOOR-01", "zones": "/t/zones.json", "background_image": "/t/empty-scene.png",
              "source": {"type": "images", "path": "/t/route"}}
    if ignore:
        camera["ignore"] = [LED_IGNORE]
    document = {"broker": {"host": "127.0.0.1", "port": 1883}, "perception_node_id": "NODE-RPI5-02",
                "contracts_dir": "/w/contracts", "simulated": True,
                "vision": {"detector": "background", "confirmation_count": 3, "lost_count": 3,
                           "republish_seconds": 0, "presence_fifo": None, "cameras": [camera]}}
    (work / name).write_text(json.dumps(document), "utf-8")
    return f"/t/{name}"


def service(work: Path, args: str) -> Any:
    return run_shell(f"{SERVICE} {args}", extra_mounts=[f"{work.resolve().as_posix()}:/t"], timeout=600)


def messages(result: Any) -> list[dict[str, Any]]:
    assert result.returncode == 0, result.stderr[-3000:]
    out = []
    for line in result.stdout.splitlines():
        topic, _, payload = line.partition(" ")
        document = json.loads(payload)
        validate_topic_message(topic, document)
        out.append(document)
    return out


@pytest.fixture(scope="module")
def scene(tmp_path_factory) -> Path:
    build(target="homemind-vision hm_vision_tests")
    work = tmp_path_factory.mktemp("vision-background")
    write_scene(work)
    return work


def test_unit_tests_pass() -> None:
    result = run_shell("/build/libs/hm_vision/hm_vision_tests --gtest_filter='Background*:Png*'")
    assert result.returncode == 0, result.stdout[-3000:]


def test_missing_empty_scene_says_what_to_run(scene: Path) -> None:
    result = service(scene, f"--config {config(scene, ignore=True, name='missing.json')} --stdout")
    if (scene / "empty-scene.png").exists():
        pytest.skip("empty scene already captured by an earlier test in this module")
    assert result.returncode == 1
    assert "--capture-background" in result.stderr


def test_route_through_the_zones(scene: Path) -> None:
    cfg = config(scene, ignore=True, name="vision.json")
    capture = service(scene, f"--config {cfg} --capture-background --images /t/empty")
    assert capture.returncode == 0, capture.stderr[-2000:]
    assert (scene / "empty-scene.png").exists()

    published = messages(service(scene, f"--config {cfg} --stdout --log-level WARNING"))
    assert published, "nothing published"
    for document in published:
        assert document["observation_type"] == "FIGURE_PRESENCE"
        assert document["attributes"]["model_version"] == "background-diff-v1"
        assert document["simulated"] is True
    present = [document["zone"] for document in published if document["attributes"]["present"]]
    order = [zone for i, zone in enumerate(present) if i == 0 or present[i - 1] != zone]
    assert order == ["entrance", "bed", "bathroom"], published
    assert all(d["attributes"]["figure_count"] == 1 for d in published if d["attributes"]["present"])
    assert published[0]["attributes"]["present"] is False   # empty room first
    assert published[-1]["attributes"]["present"] is False  # and after the figure left


def test_led_without_ignore_area_is_taken_for_a_figure(scene: Path) -> None:
    if not (scene / "empty-scene.png").exists():
        pytest.skip("needs the empty scene from test_route_through_the_zones")
    cfg = config(scene, ignore=False, name="no-ignore.json")
    published = messages(service(scene, f"--config {cfg} --stdout --log-level WARNING"))
    assert published[0]["attributes"]["present"] is True and published[0]["zone"] == "bed"
