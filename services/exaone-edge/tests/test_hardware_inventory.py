import json
from pathlib import Path


ROOT = Path(__file__).parents[1]


def load(name):
    return json.loads((ROOT / "config" / name).read_text(encoding="utf-8"))


def test_capability_ids_match_user_confirmed_hardware_table():
    devices = load("device-capabilities.json")["devices"]
    assert devices["LIGHT"]["hardware_id"] == "LED-LIVING-01"
    assert devices["ENTRY_DOOR"]["hardware_id"] == "SERVO-DOOR-01"
    assert devices["BATHROOM_DOOR"]["hardware_id"] == "SERVO-DOOR-02"
    assert devices["WINDOW"]["hardware_id"] == "SERVO-WINDOW-01"
    assert devices["VENTILATION_FAN"]["hardware_id"] == "FAN-01"
    assert devices["WASHER"]["hardware_id"] == "LED-WASHER-01"
    assert devices["AIR_CONDITIONER"]["hardware_id"] == "LED-AIRCON-01"
    assert devices["TV"]["hardware_id"] == "LED-TV-01"
    assert devices["LAPTOP"]["hardware_id"] == "LED-LAPTOP-01"


def test_motor_ids_follow_id_table():
    hardware = load("hardware-inventory.json")["hardware"]
    motors = {item["channel"]: item["id"] for item in hardware if item["type"] == "NEMA17"}
    assert motors == {"x": "MOTOR-XY-X-01", "y": "MOTOR-XY-Y-01"}


def test_inventory_matches_shared_registry():
    registry = json.loads(
        (ROOT.parent.parent / "contracts" / "pfn.v1" / "device-registry.json").read_text(encoding="utf-8")
    )
    devices = {item["device_id"]: item for item in registry["devices"]}
    hardware = load("hardware-inventory.json")["hardware"]
    ids = [item["id"] for item in hardware]
    assert len(ids) == len(set(ids))
    for item in hardware:
        device = devices[item["id"]]
        assert item["controller"] == device["controller_node_id"], item["id"]
        assert item["pin"] == device["pin"], item["id"]
        assert device["wiring_status"] == "wired", item["id"]
    perception = {item["id"] for item in hardware if item["controller"] == "NODE-RPI5-02"}
    assert perception == {"MIC-INDOOR-01", "CAM-INDOOR-01", "CAM-OUTDOOR-01"}
    assert "BG90S" not in json.dumps(load("hardware-inventory.json"))


def test_capabilities_match_action_map():
    action_map = json.loads(
        (ROOT.parent.parent / "contracts" / "pfn.v1" / "ai-action-map.json").read_text(encoding="utf-8")
    )
    for name, item in load("device-capabilities.json")["devices"].items():
        assert item["hardware_id"] == action_map["devices"][name]["device_id"], name
    zones = load("device-capabilities.json")["zones"]
    assert set(zones) <= set(action_map["zone_map"])


def test_llm_result_accepts_current_runtime_origins():
    schema = json.loads(
        (ROOT.parent.parent / "contracts" / "pfn.v1" / "llm-result.schema.json")
        .read_text(encoding="utf-8")
    )
    origins = schema["properties"]["model"]["properties"]["origin"]["enum"]
    assert "model_guard" in origins
    assert "guard_fallback" in origins
