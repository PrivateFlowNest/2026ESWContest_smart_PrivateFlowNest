// Device list for the phone app from contracts/pfn.v1/device-registry.json (registry v4).
// Port of team/homemind-application-2026-10-21/backend/app/registry.py: same names, categories, aliases and
// command mapping (servo OPEN/CLOSE via registry positions, set_switch ON/OFF, not-wired devices have none).
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"

namespace hm::mobile {

// App command -> pfn.v1 device_control parameters (core validates them again against the same registry).
struct ControlRequest {
  std::string device_id;
  std::string action;  // set_position | set_switch
  json parameters = json::object();
  std::optional<std::string> position_ref;  // open | closed for servos
};

// HardwareDevice of the Python API (hardware_models.py), same JSON fields.
struct DeviceView {
  std::string id;
  std::string name;
  std::string hardware;
  json controller_id = nullptr;
  std::string controller_name;
  json alias = nullptr;
  std::string category;
  std::string wiring_status = "wired";
  json zone = nullptr;
  bool online = false;
  std::string state = "UNKNOWN";  // last state confirmed by core (task completed = ESP ACK)
  std::vector<std::string> supported_commands;
  std::optional<std::string> pending_command;
  std::optional<std::string> pending_task_id;
  std::optional<std::string> last_task_state;
  std::optional<std::string> last_failure_code;

  json to_json() const;
};

// ComputeNode (NODE-*) or HardwareController (everything else) of the Python API.
struct NodeView {
  bool controller = false;
  std::string label;  // "symbol" for nodes, "name" for controllers (registry asset_label)
  std::string id;
  std::string hardware;
  std::string role;
  bool online = false;

  json to_json() const;
};

class UnsupportedCommand : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Registry {
 public:
  explicit Registry(const std::filesystem::path& path);

  std::vector<DeviceView> devices() const;
  void nodes(std::vector<NodeView>& nodes, std::vector<NodeView>& controllers) const;
  const json* device(const std::string& device_id) const;
  // Throws std::out_of_range (unknown device) or UnsupportedCommand ("<command> is not supported by <id>").
  ControlRequest control(const std::string& device_id, const std::string& command) const;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  json raw_;
  std::map<std::string, json> devices_;
  std::map<std::string, json> nodes_;
};

std::vector<std::string> supported_commands(const json& device);

}  // namespace hm::mobile
