#include "hm/partial/intent.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <stdexcept>
#include <utility>

#include "hm/action_map.hpp"
#include "hm/pfn.hpp"

#ifdef HM_PARTIAL_HAVE_ORT
#include <onnxruntime_cxx_api.h>
#endif

namespace hm::partial {

namespace {

// features.py SEMANTIC_TERMS (dict order), NON_ACTION_TERMS, REQUEST_TERMS; policy.py phrase lists.
// Generated from the Python modules; native/tests/test_partial_parity.py compares them on every run.
const std::vector<std::pair<std::string, std::vector<std::string>>> kSemanticTerms = {
    {"CLIMATE_COMFORT", {"춥", "쌀쌀", "으슬", "따뜻", "덥", "후끈", "시원", "냉방", "난방", "온도", "기온", "몇 도", "실내가 몇", "에어컨", "차갑", "포근", "냉기", "열기", "뜨거", "서늘", "선선", "찜통", "땀"}},
    {"LIGHTING", {"어둡", "밝", "불빛", "조명", "전등", "눈부", "깜깜", "방 불", "현관 불", "환하", "빛", "조도", "스탠드", "천장 등"}},
    {"VENTILATION", {"환기", "공기", "냄새", "탁해", "습", "눅눅", "먼지", "창문", "열린 창", "환풍", "공기청정", "매캐", "숨쉬", "수증기", "외풍", "퀴퀴"}},
    {"MEDIA", {"티비", "tv", "텔레비전", "영상", "영화", "뉴스", "드라마", "대사", "볼륨", "음량", "음소거", "방송", "프로그램", "미디어", "화면", "채널", "재생"}},
    {"ACCESS", {"현관문", "출입문", "화장실 문", "욕실", "문 상태", "현관", "출입 상태", "출입구", "문 앞", "입구", "택배 기사"}},
    {"LAUNDRY", {"세탁", "빨래", "세탁기", "옷을 넣", "옷을 다 빨", "남은 세탁"}},
    {"CLEANING", {"청소", "로봇청소기", "청소기", "방바닥", "바닥", "부스러기", "치우", "방 정리", "먼지 치우", "정리가 끝"}},
    {"QUIET_ENVIRONMENT", {"회의", "조용", "고요", "방해", "집중", "통화", "화상", "발표", "온라인 수업", "온라인 강의", "말소리", "상대방 목소리", "알림", "마이크", "수업"}},
};
const std::vector<std::string> kNonActionTerms = {"단어", "제목", "이야기", "사진", "광고", "리뷰", "영어로", "표현", "색상", "새 제품", "좋아하는 편", "친구를 만나", "메뉴", "일정", "생각이 안 나", "배경", "디자인", "배우", "노래", "인테리어", "참고용", "약자", "설명서", "뜻은 아니", "말한 게 아니", "건드리지", "그대로", "유지해", "부르기도", "제품", "지 마", "하지 마", "말고", "뜻이 아니", "부탁한 게 아니", "줄이지 않았으면"};
const std::vector<std::string> kRequestTerms = {"싶", "필요", "해야", "확인", "상태", "궁금", "좋겠", "너무", "같은데", "모르겠", "준비", "조절", "알려", "보고 싶", "켜", "끄", "열", "닫", "줄", "낮"};
const std::vector<std::string> kSuppressionPhrases = {"지 마", "지마", "하지 마", "하지마", "말고", "그대로 둬", "그대로 유지", "건드리지", "필요 없어", "필요없어", "원하지 않아", "원한 게 아니", "뜻은 아니", "뜻이 아니", "하라는 게 아니", "달라는 뜻은 아니", "부탁한 게 아니", "줄이지 않았으면", "열지 않았으면", "시작하라고 말한 게 아니"};
const std::vector<std::string> kMetaReferencePhrases = {"제품 이름", "제품 비교", "사용 설명서", "광고에", "디자인에 관해", "사진이 들어간", "사진을 인테리어", "모형을 어떻게", "제목의", "제목이", "단어의 뜻", "뜻을 설명", "약자가"};
const std::vector<std::string> kDirectActionTerms = {"켜줘", "켜고", "끄고", "꺼줘", "열어", "닫아", "작동해", "작동시켜", "시작해", "멈춰", "줄여", "낮춰", "높여", "올려", "바꿔", "바꿀", "작동해 달라고", "켜지", "열지", "시작하라고"};

const std::map<std::string, Hints> kHints = {
    {"CLIMATE_COMFORT", {{"AIR_CONDITIONER"}, {"CHECK_TEMPERATURE", "CHECK_PRESENCE", "CHECK_DEVICE_STATE"}}},
    {"LIGHTING", {{"LIGHT"}, {"CHECK_PRESENCE", "CHECK_DEVICE_STATE"}}},
    {"VENTILATION", {{"WINDOW", "VENTILATION_FAN", "AIR_PURIFIER"}, {"CHECK_AIR_QUALITY", "CHECK_OPENING_STATE", "CHECK_DEVICE_STATE"}}},
    {"MEDIA", {{"TV"}, {"CHECK_DEVICE_STATE"}}},
    {"ACCESS", {{"ENTRY_DOOR", "BATHROOM_DOOR"}, {"CHECK_OPENING_STATE", "CHECK_PRESENCE"}}},
    {"LAUNDRY", {{"WASHER"}, {"CHECK_TASK_STATE", "CHECK_DEVICE_STATE"}}},
    {"CLEANING", {{"ROBOT_VACUUM"}, {"CHECK_TASK_STATE", "CHECK_PRESENCE", "CHECK_DEVICE_STATE"}}},
    {"QUIET_ENVIRONMENT", {{"ALL_QUIET_DEVICES"}, {"WAIT_FOR_CONTEXT_EVIDENCE", "CHECK_PRESENCE"}}},
    {"NO_ACTION", {{}, {}}},
};

using Terms = std::vector<CodePoints>;

Terms to_code_points(const std::vector<std::string>& values) {
  Terms out;
  for (const auto& value : values) out.push_back(decode_utf8(value));
  return out;
}

const std::vector<Terms>& semantic_terms() {
  static const std::vector<Terms> terms = [] {
    std::vector<Terms> out;
    for (const auto& [intent, values] : kSemanticTerms) out.push_back(to_code_points(values));
    return out;
  }();
  return terms;
}

const Terms& non_action_terms() {
  static const Terms terms = to_code_points(kNonActionTerms);
  return terms;
}
const Terms& request_terms() {
  static const Terms terms = to_code_points(kRequestTerms);
  return terms;
}
const Terms& suppression_terms() {  // SUPPRESSION_PHRASES + META_REFERENCE_PHRASES
  static const Terms terms = [] {
    Terms out = to_code_points(kSuppressionPhrases);
    for (auto& value : to_code_points(kMetaReferencePhrases)) out.push_back(std::move(value));
    return out;
  }();
  return terms;
}
const Terms& direct_action_terms() {
  static const Terms terms = to_code_points(kDirectActionTerms);
  return terms;
}

std::size_t total_count(const CodePoints& text, const Terms& terms) {
  std::size_t total = 0;
  for (const auto& term : terms) total += count(text, term);
  return total;
}

bool any_contained(const CodePoints& text, const Terms& terms) {
  return std::any_of(terms.begin(), terms.end(), [&](const CodePoints& term) { return contains(text, term); });
}

bool looks_like_uuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

}  // namespace

const std::vector<std::string> kIntents = {"CLIMATE_COMFORT", "LIGHTING", "VENTILATION", "MEDIA", "ACCESS",
                                           "LAUNDRY", "CLEANING", "QUIET_ENVIRONMENT", "NO_ACTION"};

std::vector<float> vectorize_one(const CodePoints& text, int dimensions) {
  if (dimensions < 128) throw std::invalid_argument("dimensions must be at least 128");
  const CodePoints plain = normalize_text(text);
  const CodePoints normalized = U"^" + plain + U"$";
  std::vector<float> vector(static_cast<std::size_t>(dimensions), 0.0f);
  const auto hashed_dimensions = static_cast<std::uint32_t>(dimensions - 64);
  for (std::size_t size = 1; size <= 5; ++size) {
    if (normalized.size() < size) continue;
    // numpy: float32 element + float32(sign / size**0.5)
    const float plus = static_cast<float>(1.0 / std::pow(static_cast<double>(size), 0.5));
    const float minus = static_cast<float>(-1.0 / std::pow(static_cast<double>(size), 0.5));
    const std::string prefix = std::to_string(size) + ":";
    for (std::size_t start = 0; start + size <= normalized.size(); ++start) {
      const std::uint32_t hashed = fnv1a_32(prefix + encode_utf8_replace(normalized.substr(start, size)));
      const std::uint32_t index = hashed % hashed_dimensions;
      vector[index] = vector[index] + ((hashed & 0x80000000u) == 0 ? plus : minus);
    }
  }
  const std::size_t offset = hashed_dimensions;
  const auto& semantic = semantic_terms();
  for (std::size_t index = 0; index < semantic.size(); ++index) {
    const std::size_t matches = total_count(plain, semantic[index]);
    vector[offset + index * 2] = static_cast<float>(static_cast<double>(std::min<std::size_t>(matches, 3)) * 8.0);
    vector[offset + index * 2 + 1] = matches ? 8.0f : 0.0f;
  }
  const std::size_t non_action = total_count(plain, non_action_terms());
  const std::size_t request = total_count(plain, request_terms());
  vector[offset + 20] = static_cast<float>(static_cast<double>(std::min<std::size_t>(non_action, 3)) * 10.0);
  vector[offset + 21] = non_action ? 10.0f : 0.0f;
  vector[offset + 22] = static_cast<float>(static_cast<double>(std::min<std::size_t>(request, 4)) * 2.0);
  vector[offset + 23] = request ? 2.0f : 0.0f;
  vector[offset + 24] = static_cast<float>(static_cast<double>(std::min<std::size_t>(plain.size(), 100)) / 25.0);
  // np.linalg.norm (float32 dot + sqrt); the accumulation here is double, numpy's is BLAS sdot.
  double squares = 0.0;
  for (float value : vector) squares += static_cast<double>(value) * static_cast<double>(value);
  const float norm = std::sqrt(static_cast<float>(squares));
  if (norm != 0.0f) {
    for (float& value : vector) value = value / norm;
  }
  return vector;
}

std::string apply_safety_policy(const CodePoints& text, const std::string& predicted_intent) {
  return any_contained(normalize_text(text), suppression_terms()) ? std::string("NO_ACTION") : predicted_intent;
}

bool requires_final_transcript(const CodePoints& text) {
  return any_contained(normalize_text(text), direct_action_terms());
}

const Hints& hints_for(const std::string& intent) {
  const auto it = kHints.find(intent);
  if (it == kHints.end()) throw std::out_of_range("no hints for intent " + intent);
  return it->second;
}

ModelMetadata ModelMetadata::load(const std::filesystem::path& path) {
  const json raw = hm::read_json_file(path);
  ModelMetadata metadata;
  metadata.model_version = raw.at("model_version").get<std::string>();
  metadata.classes = raw.at("classes").get<std::vector<std::string>>();
  metadata.feature_dimensions = raw.at("feature_dimensions").get<int>();
  metadata.confidence_threshold = raw.at("confidence_threshold").get<double>();
  if (raw.contains("streaming_threshold")) metadata.streaming_threshold = raw["streaming_threshold"].get<double>();
  return metadata;
}

// ------------------------------------------------------------------------------------------ runtime

#ifdef HM_PARTIAL_HAVE_ORT
struct IntentRuntime::Session {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "hm_partial"};
  std::unique_ptr<Ort::Session> session;
};
#else
struct IntentRuntime::Session {};
#endif

IntentRuntime::IntentRuntime(const std::filesystem::path& model_path, const std::filesystem::path& metadata_path) {
  std::filesystem::path metadata_file = metadata_path;
  if (metadata_file.empty()) metadata_file = std::filesystem::path(model_path).replace_extension(".json");
  metadata_ = ModelMetadata::load(metadata_file);
#ifdef HM_PARTIAL_HAVE_ORT
  session_ = std::make_unique<Session>();
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(1);
  options.SetInterOpNumThreads(1);
  try {
    session_->session = std::make_unique<Ort::Session>(session_->env, model_path.c_str(), options);
  } catch (const Ort::Exception& error) {
    throw std::runtime_error(model_path.string() + ": " + error.what());
  }
#else
  throw std::runtime_error("hm_partial was built without ONNX Runtime (HM_ONNXRUNTIME_ROOT); cannot load " +
                           model_path.string());
#endif
}

IntentRuntime::~IntentRuntime() = default;

std::vector<double> IntentRuntime::probabilities(const CodePoints& text) const {
#ifdef HM_PARTIAL_HAVE_ORT
  std::vector<float> features = vectorize_one(text, metadata_.feature_dimensions);
  const std::int64_t shape[2] = {1, metadata_.feature_dimensions};
  const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input = Ort::Value::CreateTensor<float>(memory, features.data(), features.size(), shape, 2);
  const char* inputs[] = {"features"};
  const char* outputs[] = {"probabilities"};
  auto values = session_->session->Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
  const float* data = values[0].GetTensorData<float>();
  const std::size_t count = values[0].GetTensorTypeAndShapeInfo().GetElementCount();
  if (count != metadata_.classes.size()) throw std::runtime_error("model output does not match metadata classes");
  return std::vector<double>(data, data + count);
#else
  (void)text;
  throw std::runtime_error("hm_partial was built without ONNX Runtime");
#endif
}

json IntentRuntime::predict(std::string_view utf8_text, bool is_final, const std::optional<std::string>& trace_id,
                            std::optional<double> threshold) const {
  return predict(decode_utf8(utf8_text), is_final, trace_id, threshold);
}

json IntentRuntime::predict(const CodePoints& text, bool is_final, const std::optional<std::string>& trace_id,
                            std::optional<double> threshold) const {
  const CodePoints normalized = normalize_text(text);
  std::string intent;
  double confidence = 0.0;
  if (has_surrogate(normalized)) {  // malformed_text
    intent = "NO_ACTION";
    confidence = 1.0;
  } else {
    const std::vector<double> probabilities = this->probabilities(normalized);
    std::size_t best = 0;
    for (std::size_t i = 1; i < probabilities.size(); ++i) {
      if (probabilities[i] > probabilities[best]) best = i;  // max(): first maximum wins
    }
    intent = metadata_.classes[best];
    confidence = probabilities[best];
    intent = apply_safety_policy(normalized, intent);
    if (intent == "NO_ACTION") {
      const auto it = std::find(metadata_.classes.begin(), metadata_.classes.end(), "NO_ACTION");
      const double no_action = it == metadata_.classes.end() ? 0.0 : probabilities[it - metadata_.classes.begin()];
      if (no_action > confidence) confidence = no_action;
    }
  }
  const double active_threshold = threshold.value_or(metadata_.confidence_threshold);
  std::string result;
  if (intent == "NO_ACTION") {
    result = "SUPPRESSED";
  } else if (normalized.size() < 4 || confidence < active_threshold ||
             (!is_final && requires_final_transcript(normalized))) {
    result = "INSUFFICIENT_EVIDENCE";
  } else {
    result = "DETECTED";
  }
  const std::string hint_key = result == "DETECTED" ? intent : std::string("NO_ACTION");
  const Hints& hints = hints_for(hint_key);
  return {
      {"schema_version", "1.0"},
      {"event_id", hm::new_id()},
      {"trace_id", trace_id && !trace_id->empty() ? *trace_id : hm::new_id()},
      {"timestamp", utc_isoformat_now()},
      {"source", "pi5.partial-intent"},
      {"source_node_id", "NODE-RPI5-02"},
      {"text_sha256", sha256_hex(encode_utf8_replace(normalized))},
      {"text_length", normalized.size()},
      {"is_final", is_final},
      {"intent", intent},
      {"confidence", py_round(confidence, 6)},
      {"result", result},
      {"candidate_devices", hints.candidate_devices},
      {"safe_preparations", hints.safe_preparations},
      {"requires_confirmation", result == "DETECTED"},
      {"execution_allowed", false},
      {"model", {{"name", "homemind-partial-intent"}, {"version", metadata_.model_version}, {"runtime", "ONNX_RUNTIME"}}},
  };
}

// ---------------------------------------------------------------------------------------- stream gate

StreamIntentGate::StreamIntentGate(const IntentRuntime& runtime, int confirmation_updates, std::optional<double> threshold)
    : runtime_(runtime),
      confirmation_updates_(confirmation_updates),
      threshold_(threshold.value_or(runtime.metadata().streaming_threshold.value_or(runtime.threshold()))) {
  if (confirmation_updates < 1) throw std::invalid_argument("confirmation_updates must be positive");
}

void StreamIntentGate::reset() {
  candidate_.reset();
  count_ = 0;
  emitted_.reset();
}

json StreamIntentGate::update(std::string_view utf8_text, bool is_final, const std::optional<std::string>& trace_id) {
  return update(decode_utf8(utf8_text), is_final, trace_id);
}

json StreamIntentGate::update(const CodePoints& text, bool is_final, const std::optional<std::string>& trace_id) {
  json event = runtime_.predict(text, is_final, trace_id, threshold_);
  if (event["result"] != "DETECTED") {
    candidate_.reset();
    count_ = 0;
    return event;
  }
  const std::string candidate = event["intent"].get<std::string>();
  if (candidate_ && *candidate_ == candidate) {
    count_ += 1;
  } else {
    candidate_ = candidate;
    count_ = 1;
  }
  if (emitted_ && *emitted_ == candidate) {
    event["result"] = "SUPPRESSED";
    event["candidate_devices"] = json::array();
    event["safe_preparations"] = json::array();
    event["requires_confirmation"] = false;
  } else if (count_ < confirmation_updates_ && !is_final) {
    event["result"] = "INSUFFICIENT_EVIDENCE";
    event["candidate_devices"] = json::array();
    event["safe_preparations"] = json::array();
    event["requires_confirmation"] = false;
  } else {
    emitted_ = candidate;
  }
  return event;
}

UtteranceIntentStream::UtteranceIntentStream(const IntentRuntime& runtime, int confirmation_updates,
                                             std::optional<double> threshold)
    : runtime_(runtime), confirmation_updates_(confirmation_updates), threshold_(threshold) {}

json UtteranceIntentStream::update(std::string_view utf8_text, bool is_final, const std::string& utterance_id) {
  if (!gate_ || utterance_id != utterance_id_) {
    gate_ = std::make_unique<StreamIntentGate>(runtime_, confirmation_updates_, threshold_);
    utterance_id_ = utterance_id;
  }
  return gate_->update(utf8_text, is_final, utterance_id.empty() ? std::nullopt : std::optional(utterance_id));
}

// --------------------------------------------------------------------------------------- perception

std::optional<json> hint_perception(const json& event, const hm::ActionMap& action_map, bool simulated,
                                    const std::string& source_device_id) {
  if (event.value("result", std::string()) != "DETECTED" || event.value("execution_allowed", true) != false) {
    return std::nullopt;
  }
  const json& devices = action_map.raw_map().at("devices");
  std::vector<std::string> candidates;
  for (const auto& ai_device : event.at("candidate_devices")) {
    const auto entry = devices.find(ai_device.get<std::string>());
    if (entry == devices.end() || !entry->value("enabled", false)) continue;
    const auto id = entry->find("device_id");
    if (id != entry->end() && id->is_string()) candidates.push_back(id->get<std::string>());
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  const json* microphone = action_map.device(source_device_id);
  hm::pfn::PerceptionArgs args;
  args.observation_type = "PARTIAL_INTENT_HINT";
  args.zone = microphone && microphone->contains("zone") ? microphone->at("zone").get<std::string>() : "living";
  args.confidence = event.at("confidence").get<double>();
  args.attributes = {
      {"intent", event.at("intent")},
      {"result", "DETECTED"},
      {"candidate_devices", candidates},
      {"requires_confirmation", true},
      {"execution_allowed", false},
      {"model_version", event.at("model").at("version")},
  };
  args.source_device_id = source_device_id;
  const std::string trace = event.value("trace_id", std::string());
  if (looks_like_uuid(trace)) args.trace_id = trace;
  args.simulated = simulated;
  return hm::pfn::perception(args);
}

json term_tables() {
  json semantic = json::array();
  for (const auto& [intent, terms] : kSemanticTerms) semantic.push_back({intent, terms});
  json hints = json::object();
  for (const auto& [intent, value] : kHints) {
    hints[intent] = {{"candidate_devices", value.candidate_devices}, {"safe_preparations", value.safe_preparations}};
  }
  return {{"SEMANTIC_TERMS", semantic},          {"NON_ACTION_TERMS", kNonActionTerms},
          {"REQUEST_TERMS", kRequestTerms},        {"SUPPRESSION_PHRASES", kSuppressionPhrases},
          {"META_REFERENCE_PHRASES", kMetaReferencePhrases}, {"DIRECT_ACTION_TERMS", kDirectActionTerms},
          {"INTENTS", kIntents},                   {"HINTS", hints}};
}

double py_round(double value, int digits) {
  if (!std::isfinite(value)) return value;
  char text[64];
  std::snprintf(text, sizeof text, "%.*f", digits, value);  // exact decimal rounding, ties to even
  return std::strtod(text, nullptr);
}

std::string utc_isoformat_now() {
  using namespace std::chrono;
  const auto now = time_point_cast<microseconds>(system_clock::now());
  const auto micros = now.time_since_epoch().count();
  const std::time_t seconds = static_cast<std::time_t>(micros / 1000000);
  const long fraction = static_cast<long>(micros % 1000000);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  char text[96];
  if (fraction) {
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%06ld+00:00", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, fraction);
  } else {
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d+00:00", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  }
  return text;
}

}  // namespace hm::partial
