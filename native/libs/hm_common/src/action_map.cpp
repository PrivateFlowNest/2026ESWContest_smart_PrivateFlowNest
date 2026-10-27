#include "hm/action_map.hpp"

#include <set>

namespace hm {

namespace {
const std::string kNullKey("\0null", 5);  // Python None never equals a device name

std::string key_of(const json& value) { return value.is_string() ? value.get<std::string>() : kNullKey; }
}  // namespace

ActionMap::ActionMap(const std::filesystem::path& contracts_dir) {
  const auto pfn = contracts_dir / "pfn.v1";
  map_ = read_json_file(pfn / "ai-action-map.json");
  const json registry = read_json_file(pfn / "device-registry.json");
  for (const auto& item : registry.at("devices")) devices_[item.at("device_id").get<std::string>()] = item;
  for (const auto& item : map_.at("actions")) {
    actions_[{item.at("ai_device").get<std::string>(), item.at("ai_action").get<std::string>()}] = item;
  }
}

std::string ActionMap::zone(const std::string& ai_zone) const {
  const json& zones = map_.at("zone_map");
  auto it = zones.find(ai_zone.empty() ? "UNKNOWN" : ai_zone);
  return it == zones.end() ? "unknown" : it->get<std::string>();
}

const json* ActionMap::device(const std::string& device_id) const {
  auto it = devices_.find(device_id);
  return it == devices_.end() ? nullptr : &it->second;
}

std::optional<std::string> ActionMap::device_id_for(const json& ai_device, const std::string& key) const {
  const json& devices = map_.at("devices");
  auto entry = devices.find(key_of(ai_device));
  if (entry == devices.end() || !entry->is_object()) return std::nullopt;
  auto value = entry->find(key);
  if (value == entry->end() || !value->is_string()) return std::nullopt;
  return value->get<std::string>();
}

Resolution ActionMap::resolve(const json& proposal) const {
  static const std::set<std::string> kExecutable{"DEVICE_CONTROL", "CONTEXT_SUGGESTION", "DEVICE_QUERY"};
  const json intent = proposal.value("intent", json(nullptr));
  if (!intent.is_string() || !kExecutable.count(intent.get<std::string>())) {
    return {.kind = "none", .reason = intent.is_string() ? intent.get<std::string>() : "None"};
  }
  const json ai_device = proposal.value("device", json(nullptr));
  const json ai_action = proposal.value("action", json(nullptr));
  const std::string device_key = key_of(ai_device);
  const std::string action_key = key_of(ai_action);
  auto found = actions_.find({device_key, action_key});
  if (found == actions_.end()) found = actions_.find({"*", action_key});
  if (found == actions_.end()) {
    auto name = [](const json& v) { return v.is_string() ? v.get<std::string>() : std::string("None"); };
    return {.kind = "unsupported", .reason = "no mapping for " + name(ai_device) + "/" + name(ai_action)};
  }
  const json& entry = found->second;
  if (!entry.value("enabled", false)) {
    return {.kind = "unsupported", .reason = entry.value("pending_decision", std::string("disabled"))};
  }
  const std::string kind = entry.at("kind").get<std::string>();
  if (kind == "query") return {.kind = "query", .device_id = device_id_for(ai_device, "device_id")};
  if (kind == "scenario") return {.kind = "scenario", .intent = entry.at("intent").get<std::string>()};
  if (kind != "device_control") return {.kind = "unsupported", .reason = kind};

  const json& devices = map_.at("devices");
  auto device_entry = devices.find(device_key);
  if (device_entry == devices.end() || !device_entry->value("enabled", false)) {
    std::string reason = "device disabled";
    if (device_entry != devices.end()) reason = device_entry->value("reason", reason);
    return {.kind = "unsupported", .reason = reason};
  }
  const auto device_id = device_id_for(ai_device, entry.value("target", std::string("device_id")));
  if (!device_id) return {.kind = "unsupported", .reason = "no registry device"};
  json parameters = entry.value("parameters", json::object());
  if (entry.contains("position_ref")) {
    const json* registered = device(*device_id);
    const json positions = registered ? registered->value("positions", json(nullptr)) : json(nullptr);
    if (!positions.is_object() || positions.empty()) return {.kind = "unsupported", .reason = "no registered positions"};
    const std::string ref = entry.at("position_ref").get<std::string>();
    if (!positions.contains(ref)) return {.kind = "unsupported", .reason = "position " + ref + " not registered"};
    parameters["position_degrees"] = positions.at(ref);
  }
  if (entry.contains("parameter_from")) {
    const json given = proposal.contains("parameters") && proposal["parameters"].is_object() ? proposal["parameters"]
                                                                                             : json::object();
    for (const auto& [source, target] : entry["parameter_from"].items()) {
      auto value = given.find(source);
      if (value == given.end() || value->is_null()) {
        return {.kind = "unsupported", .reason = "missing parameter " + source};
      }
      parameters[target.get<std::string>()] = *value;
    }
  }
  return {.kind = "device_control",
          .device_id = device_id,
          .command = entry.at("command").get<std::string>(),
          .parameters = parameters};
}

bool ActionMap::is_door_open(const Resolution& resolution) const {
  if (resolution.kind != "device_control" || !resolution.device_id) return false;
  const json* registered = device(*resolution.device_id);
  if (!registered) return false;
  const json positions = registered->value("positions", json::object());
  if (!positions.is_object() || !positions.contains("open")) return false;
  return resolution.command == "set_position" && resolution.parameters.contains("position_degrees") &&
         resolution.parameters["position_degrees"] == positions["open"];
}

json control_parameters(const Resolution& resolution, const std::string& origin, const json& proposal) {
  return {
      {"target_device_id", resolution.device_id ? json(*resolution.device_id) : json(nullptr)},
      {"action", resolution.command ? json(*resolution.command) : json(nullptr)},
      {"parameters", resolution.parameters},
      {"origin", origin},
      {"ai_device", proposal.value("device", json(nullptr))},
      {"ai_action", proposal.value("action", json(nullptr))},
  };
}

json scenario_parameters(const std::string& origin, const json& proposal) {
  return {
      {"origin", origin},
      {"ai_device", proposal.value("device", json(nullptr))},
      {"ai_action", proposal.value("action", json(nullptr))},
  };
}

}  // namespace hm
