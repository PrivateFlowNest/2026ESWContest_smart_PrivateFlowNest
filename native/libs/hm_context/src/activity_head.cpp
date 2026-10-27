#include "hm/context/activity_head.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace hm::ctx {
namespace {

bool is_activity_label(const std::string& label) {
  return std::find(std::begin(kActivityLabels), std::end(kActivityLabels), label) != std::end(kActivityLabels);
}

std::vector<double> doubles(const json& value, std::size_t expected, const char* name) {
  auto out = value.get<std::vector<double>>();
  if (out.size() != expected) throw std::runtime_error(std::string("activity head: bad ") + name + " size");
  return out;
}

}  // namespace

ActivityHead::ActivityHead(const std::filesystem::path& path) {
  const json document = read_json_file(path);
  if (document.value("format", "") != "hm-activity-head/1")
    throw std::runtime_error(path.string() + ": not an hm-activity-head/1 export");
  head_ = document.at("head").get<std::string>();
  model_version_ = document.at("model_version").get<std::string>();
  feature_version_ = document.at("feature_version").get<std::string>();
  classes_ = document.at("classes").get<std::vector<std::string>>();
  n_features_ = document.at("n_features").get<std::size_t>();
  weights_ = document.at("ensemble").at("weights").get<std::vector<double>>();
  const json& members = document.at("members");
  if (classes_.size() < 2 || members.size() != weights_.size() || members.empty())
    throw std::runtime_error(path.string() + ": inconsistent activity head export");
  if (head_ == "activity")
    for (const auto& label : classes_)
      if (!is_activity_label(label)) throw std::runtime_error(path.string() + ": unknown activity label " + label);
  const std::size_t rows = classes_.size() == 2 ? 1 : classes_.size();
  for (const json& item : members) {
    Member member;
    member.mean = doubles(item.at("scaler").at("mean"), n_features_, "scaler.mean");
    member.scale = doubles(item.at("scaler").at("scale"), n_features_, "scaler.scale");
    const json& logistic = item.at("logistic");
    member.intercept = doubles(logistic.at("intercept"), rows, "intercept");
    const std::string proba = logistic.at("proba").get<std::string>();
    if (proba != "softmax" && proba != "binary_logistic")
      throw std::runtime_error(path.string() + ": unsupported proba " + proba);
    member.softmax = proba == "softmax";
    if (member.softmax == (classes_.size() == 2))
      throw std::runtime_error(path.string() + ": proba does not match the class count");
    for (const json& row : logistic.at("coef")) member.coef.push_back(doubles(row, n_features_, "coef row"));
    if (member.coef.size() != rows) throw std::runtime_error(path.string() + ": bad coef rows");
    members_.push_back(std::move(member));
  }
}

std::vector<double> ActivityHead::predict_proba(const float* features, std::size_t size) const {
  if (size != n_features_)
    throw std::invalid_argument("activity head expects " + std::to_string(n_features_) + " features, got " +
                                std::to_string(size));
  std::vector<double> total(classes_.size(), 0.0);
  std::vector<float> scaled(n_features_);
  for (std::size_t m = 0; m < members_.size(); ++m) {
    const Member& member = members_[m];
    // StandardScaler.transform on float32 input: computed in float64, stored back as float32.
    for (std::size_t j = 0; j < n_features_; ++j) {
      const auto centred = static_cast<float>(static_cast<double>(features[j]) - member.mean[j]);
      scaled[j] = static_cast<float>(static_cast<double>(centred) / member.scale[j]);
    }
    std::vector<double> logits(member.coef.size());
    for (std::size_t c = 0; c < member.coef.size(); ++c) {
      const std::vector<double>& row = member.coef[c];
      double dot = 0.0;
      for (std::size_t j = 0; j < n_features_; ++j) dot += static_cast<double>(scaled[j]) * row[j];
      logits[c] = dot + member.intercept[c];
    }
    std::vector<double> p(classes_.size());
    if (member.softmax) {  // sklearn.utils.extmath.softmax
      const double peak = *std::max_element(logits.begin(), logits.end());
      double sum = 0.0;
      for (std::size_t c = 0; c < logits.size(); ++c) {
        p[c] = std::exp(logits[c] - peak);
        sum += p[c];
      }
      for (double& value : p) value /= sum;
    } else {  // binary: expit(decision) -> [1 - p, p]
      const double positive = 1.0 / (1.0 + std::exp(-logits[0]));
      p = {1.0 - positive, positive};
    }
    for (std::size_t c = 0; c < p.size(); ++c) total[c] += weights_[m] * p[c];
  }
  return total;
}

Prediction ActivityHead::predict(const float* features, std::size_t size) const {
  const std::vector<double> probabilities = predict_proba(features, size);
  const auto best = static_cast<std::size_t>(
      std::distance(probabilities.begin(), std::max_element(probabilities.begin(), probabilities.end())));
  Prediction prediction;
  prediction.head = head_;
  prediction.label = classes_[best];
  prediction.confidence = probabilities[best];
  prediction.model_version = model_version_;
  for (std::size_t c = 0; c < classes_.size(); ++c) {
    if (!(probabilities[c] >= 0.0 && probabilities[c] <= 1.0))
      throw std::runtime_error("class probabilities must be between 0 and 1");
    prediction.class_probabilities.emplace_back(classes_[c], probabilities[c]);
  }
  return prediction;
}

json prediction_to_feature_event(const Prediction& prediction, const std::string& trace_id, double speech_ratio,
                                 double duration_seconds, Micros now, int ttl_seconds, bool simulated) {
  if (prediction.head != "activity")
    throw std::invalid_argument("only activity predictions can become AUDIO_CONTEXT features");
  if (!(speech_ratio >= 0.0 && speech_ratio <= 1.0))
    throw std::invalid_argument("speech_ratio must be between 0 and 1");
  if (duration_seconds < 0 || ttl_seconds < 1)
    throw std::invalid_argument("duration_seconds must be non-negative and ttl positive");
  json payload = {
      {"context_type", prediction.label},
      {"confidence", py_round(prediction.confidence, 6)},
      {"speech_ratio", py_round(speech_ratio, 6)},
      {"duration_seconds", py_round(duration_seconds, 3)},
      {"model_version", prediction.model_version},
  };
  if (!prediction.class_probabilities.empty()) {
    json probabilities = json::object();
    for (const auto& [label, probability] : prediction.class_probabilities)
      probabilities[label] = py_round(probability, 6);
    payload["class_probabilities"] = probabilities;
  }
  return {
      {"schema_version", "1.0"},
      {"event_id", "audio-feature-" + new_id()},
      {"trace_id", trace_id},
      {"timestamp", iso_python(now)},
      {"valid_until", iso_python(now + static_cast<Micros>(ttl_seconds) * 1000000)},
      {"source", "pi5.audio-context"},
      {"source_node_id", "NODE-RPI5-02"},
      {"event_type", "AUDIO_CONTEXT"},
      {"simulated", simulated},
      {"payload", payload},
  };
}

}  // namespace hm::ctx
