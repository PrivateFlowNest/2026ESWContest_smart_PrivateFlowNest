#include "registry.hpp"

#include <set>
#include <stdexcept>

namespace hm::mobile {

namespace {

const std::map<std::string, std::string>& names() {
  static const std::map<std::string, std::string> table = {
      {"entrance_door", "현관문"},
      {"bathroom_door", "화장실 문"},
      {"window", "창문"},
      {"ventilation_fan", "환기팬"},
      {"washer_indicator", "세탁기 표시등"},
      {"air_conditioner_indicator", "에어컨 표시등"},
      {"tv_indicator", "TV 표시등"},
      {"laptop_indicator", "노트북 표시등"},
      {"living_light", "거실 조명"},
      {"night_guide_light", "야간 안내등"},
      {"bed_curtain", "침대 커튼"},
      {"washer_sound_display", "세탁기 숫자 표시"},
      {"air_conditioner_sound_display", "에어컨 숫자 표시"},
      {"tv_sound_display", "TV 숫자 표시"},
      {"voice_recognition", "실내 마이크"},
      {"indoor_camera", "실내 카메라"},
      {"outdoor_camera", "현관 밖 카메라"},
      {"xy_axis_x", "X축 이동"},
      {"xy_axis_y", "Y축 이동"},
      {"x_axis_home", "X축 원점 센서"},
      {"y_axis_home", "Y축 원점 센서"},
  };
  return table;
}

const std::map<std::string, std::string>& roles() {
  static const std::map<std::string, std::string> table = {
      {"core", "백엔드: MQTT 브로커, core, 규칙·정책"},
      {"octopus_klipper_host", "Octopus Klipper 호스트(HomeMind와 분리)"},
      {"ui_and_llm", "7인치 UI와 EXAONE"},
      {"perception", "인식: 음성·소리 상황·카메라"},
      {"door_and_window_controller", "현관문·창문 제어"},
      {"light_and_fan_controller", "거실 조명·에어컨 표시등·환기팬 제어"},
      {"xy_motion_controller", "인형 XY 이동"},
  };
  return table;
}

const std::map<std::string, std::string>& hardware_names() {
  static const std::map<std::string, std::string> table = {
      {"pi4", "Raspberry Pi 4"}, {"pi5", "Raspberry Pi 5"}, {"esp32", "ESP32"}, {"esp8266", "ESP8266"},
      {"octopus", "BTT Octopus V1.1"}};
  return table;
}

// Device hardware shown as is (MG90S, LED, ...) except these registry codes.
const std::map<std::string, std::string>& device_hardware_names() {
  static const std::map<std::string, std::string> table = {{"OPEN_CLOSE_SERVO", "열기/닫기 모터"}};
  return table;
}

// Kept in the registry but not shown in the app (registry v4, 2026-10-25): the doll axis plays the person and
// is not a home device (Octopus host and controller, XY motors, home sensors), and the removed bathroom door.
bool hidden_role(const std::string& role) {
  return role == "octopus_klipper_host" || role == "xy_motion_controller";
}

// dict.get(key, default) for string values; non-string values are returned as their JSON text.
std::string text(const json& object, const char* key, const std::string& fallback = "") {
  if (!object.is_object()) return fallback;
  auto it = object.find(key);
  if (it == object.end()) return fallback;
  return it->is_string() ? it->get<std::string>() : it->dump();
}

std::string lookup(const std::map<std::string, std::string>& table, const std::string& key,
                   const std::string& fallback) {
  auto it = table.find(key);
  return it == table.end() ? fallback : it->second;
}

std::set<std::string> capabilities(const json& device) {
  std::set<std::string> out;
  if (device.contains("capabilities") && device["capabilities"].is_array()) {
    for (const auto& item : device["capabilities"]) {
      if (item.is_string()) out.insert(item.get<std::string>());
    }
  }
  return out;
}

// Python truthiness of a JSON value (None, "", 0, {}, [] are false).
bool truthy(const json& value) {
  if (value.is_null()) return false;
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number()) return value.get<double>() != 0.0;
  if (value.is_string() || value.is_array() || value.is_object()) return !value.empty();
  return true;
}

std::string category(const json& device) {
  const auto caps = capabilities(device);
  const std::string id = text(device, "device_id");
  if (caps.count("set_position") || id.rfind("FAN-", 0) == 0) return "actuator";
  if (caps.count("set_switch") || caps.count("set_color") || caps.count("set_number")) return "indicator";
  if (caps.count("move_axis") || caps.count("home_axis")) return "motion";
  if (caps.count("binary_state")) return "sensor";
  return "input";
}

json optional_json(const std::optional<std::string>& value) { return value ? json(*value) : json(nullptr); }

}  // namespace

std::vector<std::string> supported_commands(const json& device) {
  if (!device.is_object() || device.value("wiring_status", json(nullptr)) != "wired") return {};
  const auto caps = capabilities(device);
  if (caps.count("set_position") && device.contains("positions") && truthy(device["positions"])) {
    return {"OPEN", "CLOSE"};
  }
  if (caps.count("set_switch")) return {"ON", "OFF"};
  return {};
}

json DeviceView::to_json() const {
  return {
      {"id", id},
      {"name", name},
      {"hardware", hardware},
      {"controller_id", controller_id},
      {"controller_name", controller_name},
      {"alias", alias},
      {"category", category},
      {"wiring_status", wiring_status},
      {"zone", zone},
      {"online", online},
      {"state", state},
      {"supported_commands", supported_commands},
      {"pending_command", optional_json(pending_command)},
      {"pending_task_id", optional_json(pending_task_id)},
      {"last_task_state", optional_json(last_task_state)},
      {"last_failure_code", optional_json(last_failure_code)},
  };
}

json NodeView::to_json() const {
  return {{controller ? "name" : "symbol", label}, {"id", id}, {"hardware", hardware}, {"role", role},
          {"online", online}};
}

Registry::Registry(const std::filesystem::path& path) : path_(path), raw_(read_json_file(path)) {
  if (!raw_.is_object() || !raw_.contains("devices") || !raw_["devices"].is_array() || !raw_.contains("nodes") ||
      !raw_["nodes"].is_array()) {
    throw std::runtime_error(path.string() + ": not a device registry (devices/nodes missing)");
  }
  for (const auto& device : raw_["devices"]) devices_[device.at("device_id").get<std::string>()] = device;
  for (const auto& node : raw_["nodes"]) nodes_[node.at("node_id").get<std::string>()] = node;
}

std::vector<DeviceView> Registry::devices() const {
  std::vector<DeviceView> out;
  for (const auto& device : raw_["devices"]) {
    const std::string controller_id = text(device, "controller_node_id");
    auto node = nodes_.find(controller_id);
    if (text(device, "assignment_status") == "removed_from_model" ||
        (node != nodes_.end() && hidden_role(text(node->second, "role")))) {
      continue;
    }
    DeviceView view;
    view.id = device.at("device_id").get<std::string>();
    view.name = lookup(names(), text(device, "function"), view.id);
    const std::string hardware = text(device, "hardware");
    view.hardware = lookup(device_hardware_names(), hardware, hardware);
    view.controller_id = device.value("controller_node_id", json(nullptr));
    const std::string node_type = node == nodes_.end() ? "" : text(node->second, "node_type");
    view.controller_name = lookup(hardware_names(), node_type, controller_id);
    // device.get("channel") or device.get("pin")
    const json channel = device.value("channel", json(nullptr));
    view.alias = truthy(channel) ? channel : device.value("pin", json(nullptr));
    view.category = category(device);
    view.wiring_status = text(device, "wiring_status", "unknown");
    view.zone = device.value("zone", json(nullptr));
    view.supported_commands = supported_commands(device);
    out.push_back(std::move(view));
  }
  return out;
}

void Registry::nodes(std::vector<NodeView>& nodes, std::vector<NodeView>& controllers) const {
  nodes.clear();
  controllers.clear();
  for (const auto& node : raw_["nodes"]) {
    const std::string role = text(node, "role");
    if (hidden_role(role)) continue;
    NodeView view;
    view.id = node.at("node_id").get<std::string>();
    const std::string node_type = text(node, "node_type");
    view.hardware = lookup(hardware_names(), node_type, node_type);
    view.role = lookup(roles(), role, role);
    view.label = text(node, "asset_label");
    view.controller = view.id.rfind("NODE-", 0) != 0;
    (view.controller ? controllers : nodes).push_back(std::move(view));
  }
}

const json* Registry::device(const std::string& device_id) const {
  auto it = devices_.find(device_id);
  return it == devices_.end() ? nullptr : &it->second;
}

ControlRequest Registry::control(const std::string& device_id, const std::string& command) const {
  const json& device = devices_.at(device_id);
  const auto commands = supported_commands(device);
  if (std::find(commands.begin(), commands.end(), command) == commands.end()) {
    throw UnsupportedCommand(command + " is not supported by " + device_id);
  }
  if (command == "OPEN" || command == "CLOSE") {
    const std::string ref = command == "OPEN" ? "open" : "closed";
    return {device_id, "set_position", {{"position_degrees", device.at("positions").at(ref)}}, ref};
  }
  return {device_id, "set_switch", {{"on", command == "ON"}}, std::nullopt};
}

}  // namespace hm::mobile
