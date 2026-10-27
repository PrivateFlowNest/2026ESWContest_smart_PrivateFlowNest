#include "hm/vision/zones.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hm::vision {

namespace {
constexpr std::array<const char*, 6> kAllowedZones = {"BED", "DESK", "BATHROOM", "ENTRANCE", "COMMON", "OUTDOOR"};

// str(value) for the JSON scalars a config file can hold (camera_id).
std::string python_str(const nlohmann::ordered_json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_null()) return "None";
  if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
  return value.dump();
}
}  // namespace

bool is_allowed_zone(const std::string& name) {
  for (const char* zone : kAllowedZones) {
    if (name == zone) return true;
  }
  return false;
}

BoundingBox BoundingBox::make(double x1, double y1, double x2, double y2, double confidence) {
  if (!(0.0 <= x1 && x1 < x2 && x2 <= 1.0 && 0.0 <= y1 && y1 < y2 && y2 <= 1.0)) {
    throw std::invalid_argument("bounding box coordinates must be normalized and ordered");
  }
  if (!(0.0 <= confidence && confidence <= 1.0)) throw std::invalid_argument("confidence must be between 0 and 1");
  return {x1, y1, x2, y2, confidence};
}

bool point_in_polygon(std::pair<double, double> point, const Polygon& polygon) {
  const auto [x, y] = point;
  bool inside = false;
  auto previous = polygon.back();
  for (const auto& current : polygon) {
    const double x1 = previous.first, y1 = previous.second;
    const double x2 = current.first, y2 = current.second;
    if ((y1 > y) != (y2 > y)) {
      const double crossing_x = (x2 - x1) * (y - y1) / (y2 - y1) + x1;
      if (x <= crossing_x) inside = !inside;
    }
    previous = current;
  }
  return inside;
}

ZoneMap::ZoneMap(std::string camera_id, std::vector<std::pair<std::string, Polygon>> zones) {
  if (camera_id.empty()) throw std::invalid_argument("camera_id is required");
  for (const auto& [name, polygon] : zones) {
    if (!is_allowed_zone(name)) throw std::invalid_argument("unsupported zone: " + name);
    if (polygon.size() < 3) throw std::invalid_argument("zone " + name + " requires at least three points");
    for (const auto& [x, y] : polygon) {
      if (!(0.0 <= x && x <= 1.0 && 0.0 <= y && y <= 1.0)) {
        throw std::invalid_argument("zone " + name + " coordinates must be normalized");
      }
    }
  }
  camera_id_ = std::move(camera_id);
  zones_ = std::move(zones);
}

ZoneMap ZoneMap::from_json(const nlohmann::ordered_json& value) {
  if (!value.is_object() || !value.contains("zones") || !value["zones"].is_object()) {
    throw std::invalid_argument("zone configuration must contain a zones object");
  }
  std::vector<std::pair<std::string, Polygon>> zones;
  for (const auto& [name, points] : value["zones"].items()) {
    Polygon polygon;
    if (!points.is_array()) throw std::invalid_argument("zone " + name + " must be a list of points");
    for (const auto& point : points) {
      if (!point.is_array() || point.size() < 2 || !point[0].is_number() || !point[1].is_number()) {
        throw std::invalid_argument("zone " + name + " has a malformed point");
      }
      polygon.emplace_back(point[0].get<double>(), point[1].get<double>());
    }
    zones.emplace_back(name, std::move(polygon));
  }
  return ZoneMap(value.contains("camera_id") ? python_str(value["camera_id"]) : std::string(), std::move(zones));
}

ZoneMap ZoneMap::load(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot read " + path.string());
  std::stringstream buffer;
  buffer << stream.rdbuf();
  std::string text = buffer.str();
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
  try {
    return from_json(nlohmann::ordered_json::parse(text));
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error(path.string() + ": " + error.what());
  }
}

std::string ZoneMap::locate(const BoundingBox& box) const {
  for (const auto& [name, polygon] : zones_) {
    if (point_in_polygon(box.foot_point(), polygon)) return name;
  }
  return "UNKNOWN";
}

ZoneStabilizer::ZoneStabilizer(int confirmation_count, int lost_count)
    : confirmation_count_(confirmation_count), lost_count_(lost_count) {
  if (confirmation_count < 1 || lost_count < 1) {
    throw std::invalid_argument("confirmation_count and lost_count must be positive");
  }
}

std::optional<std::string> ZoneStabilizer::update(const std::optional<std::string>& zone) {
  const std::string observed = zone && !zone->empty() ? *zone : "UNKNOWN";
  if (observed != "UNKNOWN") {
    lost_ = 0;
  } else if (current_ != "UNKNOWN") {
    lost_ += 1;
    if (lost_ < lost_count_) return std::nullopt;
    current_ = "UNKNOWN";
    candidate_ = "UNKNOWN";
    candidate_count_ = 0;
    return current_;
  }
  if (observed == current_) {
    candidate_ = observed;
    candidate_count_ = 0;
    return std::nullopt;
  }
  if (observed != candidate_) {
    candidate_ = observed;
    candidate_count_ = 1;
  } else {
    candidate_count_ += 1;
  }
  if (candidate_count_ < confirmation_count_) return std::nullopt;
  current_ = observed;
  candidate_count_ = 0;
  return current_;
}

}  // namespace hm::vision
