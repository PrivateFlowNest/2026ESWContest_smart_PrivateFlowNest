// AI proposal -> core request, from contracts/pfn.v1/ai-action-map.json + device-registry.json.
// Port of services/runtime action_map.py (same resolutions and reasons).
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "hm/common.hpp"

namespace hm {

struct Resolution {
  std::string kind;  // device_control | scenario | query | unsupported | none
  std::optional<std::string> device_id;
  std::optional<std::string> command;
  json parameters = json::object();
  std::optional<std::string> intent;
  std::optional<std::string> reason;
};

class ActionMap {
 public:
  explicit ActionMap(const std::filesystem::path& contracts_dir);

  std::string zone(const std::string& ai_zone) const;  // AI zone -> registry zone ("unknown" if absent)
  Resolution resolve(const json& proposal) const;
  bool is_door_open(const Resolution& resolution) const;
  const json& raw_map() const { return map_; }
  const json* device(const std::string& device_id) const;

 private:
  std::optional<std::string> device_id_for(const json& ai_device, const std::string& key) const;

  json map_;
  std::map<std::string, json> devices_;
  std::map<std::pair<std::string, std::string>, json> actions_;
};

// parameters for intent device_control (contracts/pfn.v1/device-control.schema.json)
json control_parameters(const Resolution& resolution, const std::string& origin, const json& proposal);
json scenario_parameters(const std::string& origin, const json& proposal);

}  // namespace hm
