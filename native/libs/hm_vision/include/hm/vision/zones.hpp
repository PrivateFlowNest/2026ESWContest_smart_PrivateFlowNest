// Zone judgment from the bottom-centre ("foot") point of a normalized box. Port of
// services/edge-ai/vision-presence/src/homemind_vision/zones.py (ZoneMap, ZoneStabilizer), including its
// validation messages and the polygon order of the configuration file (first matching zone wins).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace hm::vision {

// ALLOWED_ZONES (AI zone names; mapped to registry zones with hm::ActionMap::zone).
bool is_allowed_zone(const std::string& name);

struct BoundingBox {
  double x1, y1, x2, y2, confidence;
  // Same checks as BoundingBox.__post_init__; throws std::invalid_argument.
  static BoundingBox make(double x1, double y1, double x2, double y2, double confidence);
  std::pair<double, double> foot_point() const { return {(x1 + x2) / 2.0, y2}; }
};

using Polygon = std::vector<std::pair<double, double>>;
bool point_in_polygon(std::pair<double, double> point, const Polygon& polygon);

class ZoneMap {
 public:
  ZoneMap(std::string camera_id, std::vector<std::pair<std::string, Polygon>> zones);
  // {"camera_id": ..., "zones": {"BED": [[x, y], ...], ...}} with the file's key order kept.
  static ZoneMap from_json(const nlohmann::ordered_json& value);
  static ZoneMap load(const std::filesystem::path& path);

  std::string locate(const BoundingBox& box) const;  // zone name or "UNKNOWN"
  const std::string& camera_id() const { return camera_id_; }
  const std::vector<std::pair<std::string, Polygon>>& zones() const { return zones_; }

 private:
  std::string camera_id_;
  std::vector<std::pair<std::string, Polygon>> zones_;
};

// Emit a change only after the same zone is observed `confirmation_count` times; drop to UNKNOWN after
// `lost_count` consecutive misses.
class ZoneStabilizer {
 public:
  explicit ZoneStabilizer(int confirmation_count = 3, int lost_count = 3);
  // zone: nullopt / "" / "UNKNOWN" mean "not seen". Returns the new stable zone when it changes.
  std::optional<std::string> update(const std::optional<std::string>& zone);
  const std::string& current() const { return current_; }

 private:
  int confirmation_count_;
  int lost_count_;
  std::string current_ = "UNKNOWN";
  std::string candidate_ = "UNKNOWN";
  int candidate_count_ = 0;
  int lost_ = 0;
};

}  // namespace hm::vision
