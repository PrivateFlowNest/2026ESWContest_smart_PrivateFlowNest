import json
import re
from pathlib import Path

package_dir = Path(__file__).resolve().parents[1]
# Canonical contracts: <repo>/contracts/pfn.v1 (moved 2026-10-21)
registry_path = package_dir.parents[2] / "contracts" / "pfn.v1" / "device-registry.json"
catalog_path = package_dir / "lib" / "src" / "device_catalog.dart"

registry = json.loads(registry_path.read_text(encoding="utf-8"))
expected = {item["device_id"] for item in registry["devices"]}
catalog_text = catalog_path.read_text(encoding="utf-8")
actual_list = re.findall(r"OfficialDevice\(\s*id:\s*'([^']+)'", catalog_text)
actual = set(actual_list)

missing = sorted(expected - actual)
extra = sorted(actual - expected)
if missing or extra:
    raise SystemExit(f"Flutter device catalog mismatch: missing={missing}, extra={extra}")
if len(actual_list) != len(expected):
    raise SystemExit("Flutter device catalog contains duplicate IDs")

# registry v3+: controller node, wiring_status and servo positions must match too.
wiring_names = {"wired": "wired", "not_wired": "notWired", "to_confirm": "toConfirm"}
blocks = {}
for block in catalog_text.split("OfficialDevice(")[1:]:
    match = re.match(r"\s*id:\s*'([^']+)'", block)
    if match:
        blocks[match.group(1)] = block.split("];", 1)[0]

problems = []
for item in registry["devices"]:
    device_id = item["device_id"]
    block = blocks[device_id]
    controller = re.search(r"controllerNodeId:\s*'([^']+)'", block)
    if not controller or controller.group(1) != item["controller_node_id"]:
        problems.append(f"{device_id}: controller {controller and controller.group(1)} != {item['controller_node_id']}")
    wiring = re.search(r"wiring:\s*WiringStatus\.(\w+)", block)
    wiring_value = wiring.group(1) if wiring else "wired"
    if wiring_value != wiring_names[item["wiring_status"]]:
        problems.append(f"{device_id}: wiring {wiring_value} != {item['wiring_status']}")
    positions = item.get("positions")
    if positions:
        open_match = re.search(r"openDegrees:\s*(\d+)", block)
        closed_match = re.search(r"closedDegrees:\s*(\d+)", block)
        if not open_match or int(open_match.group(1)) != positions["open"]:
            problems.append(f"{device_id}: open position differs from registry")
        if not closed_match or int(closed_match.group(1)) != positions["closed"]:
            problems.append(f"{device_id}: closed position differs from registry")
if problems:
    raise SystemExit("Flutter device catalog mismatch: " + "; ".join(problems))

print(
    f"FLUTTER_DEVICE_CATALOG_PASS ({len(actual)} official IDs, "
    f"registry v{registry['registry_version']}: controller, wiring_status, positions)"
)
