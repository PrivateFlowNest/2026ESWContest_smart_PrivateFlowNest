import json

import pytest

from homemind_vision.zones import BoundingBox, ZoneMap, ZoneStabilizer


def test_bottom_center_selects_zone():
    zones = ZoneMap(
        "CAM-INDOOR-01",
        {
            "BED": ((0, 0), (0.5, 0), (0.5, 1), (0, 1)),
            "DESK": ((0.5, 0), (1, 0), (1, 1), (0.5, 1)),
        },
    )
    assert zones.locate(BoundingBox(0.1, 0.1, 0.3, 0.9, 0.95)) == "BED"
    assert zones.locate(BoundingBox(0.7, 0.1, 0.9, 0.9, 0.95)) == "DESK"


def test_unknown_is_returned_outside_polygons():
    zones = ZoneMap("CAM-OUTDOOR-01", {"OUTDOOR": ((0, 0), (0.4, 0), (0.4, 1), (0, 1))})
    assert zones.locate(BoundingBox(0.7, 0.1, 0.9, 0.9, 0.8)) == "UNKNOWN"


def test_zone_transition_requires_confirmation_and_loss_is_debounced():
    stable = ZoneStabilizer(confirmation_count=3, lost_count=2)
    assert stable.update("DESK") is None
    assert stable.update("DESK") is None
    assert stable.update("DESK") == "DESK"
    assert stable.update(None) is None
    assert stable.current == "DESK"
    assert stable.update(None) == "UNKNOWN"


def test_invalid_box_and_zone_are_rejected():
    with pytest.raises(ValueError):
        BoundingBox(0.9, 0.1, 0.2, 0.5, 0.8)
    with pytest.raises(ValueError):
        ZoneMap("CAM", {"KITCHEN": ((0, 0), (1, 0), (1, 1))})


def test_template_loads(tmp_path):
    path = tmp_path / "zones.json"
    path.write_text(
        json.dumps({"camera_id": "CAM-INDOOR-01", "zones": {"COMMON": [[0, 0], [1, 0], [1, 1], [0, 1]]}}),
        encoding="utf-8",
    )
    assert ZoneMap.load(path).camera_id == "CAM-INDOOR-01"
