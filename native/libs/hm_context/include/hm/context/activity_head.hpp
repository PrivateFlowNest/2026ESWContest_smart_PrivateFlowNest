// Activity head exported from activity.joblib by tools/export_activity_head.py (StandardScaler + multinomial
// LogisticRegression members, weighted probability ensemble) and the AUDIO_CONTEXT feature-event builder
// (homemind_audio.models.prediction_to_feature_event).
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "hm/common.hpp"
#include "hm/context/pyfmt.hpp"

namespace hm::ctx {

// homemind_audio.labels
inline constexpr const char* kActivityLabels[] = {"QUIET",           "SELF_TALK",       "TV_MEDIA", "ZOOM_SPEAKER",
                                                  "ZOOM_HEADPHONES", "IN_PERSON_MEETING", "UNKNOWN"};

struct Prediction {
  std::string head = "activity";
  std::string label;
  double confidence = 0.0;
  std::string model_version;
  std::vector<std::pair<std::string, double>> class_probabilities;  // classes_ order
};

class ActivityHead {
 public:
  explicit ActivityHead(const std::filesystem::path& path);
  // Same as _LoadedHead.predict (argmax over predict_proba, first maximum wins).
  Prediction predict(const float* features, std::size_t size) const;
  std::vector<double> predict_proba(const float* features, std::size_t size) const;

  const std::string& head() const { return head_; }
  const std::string& model_version() const { return model_version_; }
  const std::string& feature_version() const { return feature_version_; }
  const std::vector<std::string>& classes() const { return classes_; }
  std::size_t n_features() const { return n_features_; }

 private:
  struct Member {
    std::vector<double> mean, scale, intercept;
    std::vector<std::vector<double>> coef;
    bool softmax = true;
  };
  std::string head_, model_version_, feature_version_;
  std::vector<std::string> classes_;
  std::size_t n_features_ = 0;
  std::vector<double> weights_;
  std::vector<Member> members_;
};

// prediction_to_feature_event: rounds like Python (6 / 3 decimals), ISO timestamps like datetime.isoformat().
json prediction_to_feature_event(const Prediction& prediction, const std::string& trace_id, double speech_ratio,
                                 double duration_seconds, Micros now, int ttl_seconds, bool simulated);

}  // namespace hm::ctx
