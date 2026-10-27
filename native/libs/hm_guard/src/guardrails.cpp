#include "hm/guardrails.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hm/pyregex.hpp"
#include "hm/text.hpp"

namespace hm::guard {

namespace {

using py::Regex;
using Words = std::vector<std::string_view>;

// Same order as the Python dict: it decides the order of `devices` and therefore which device is reported.
const std::vector<std::pair<std::string_view, Words>>& device_aliases() {
  static const std::vector<std::pair<std::string_view, Words>> aliases = {
      {"LIGHT", {"불", "조명", "전등"}},
      {"TV", {"tv", "티비", "티브이", "텔레비전", "텔레비"}},
      {"WASHER", {"세탁기", "세탁"}},
      {"WINDOW", {"창문"}},
      {"AIR_PURIFIER", {"공기청정기", "공청기"}},
      {"ENTRY_DOOR", {"현관문", "현관 문", "출입문", "대문"}},
      {"BATHROOM_DOOR", {"화장실문", "화장실 문", "욕실문", "욕실 문"}},
      {"VENTILATION_FAN", {"환기팬", "환풍기", "환기 팬"}},
      {"AIR_CONDITIONER", {"에어컨", "에어콘", "냉방"}},
      {"LAPTOP", {"노트북"}},
      {"ROBOT_VACUUM", {"로봇청소기", "로봇 청소기"}},
      {"NOTIFICATION", {"알림"}},
  };
  return aliases;
}

std::string_view device_label(std::string_view device) {
  static const std::vector<std::pair<std::string_view, std::string_view>> labels = {
      {"LIGHT", "조명"},         {"TV", "TV"},
      {"WASHER", "세탁기"},      {"WINDOW", "창문"},
      {"AIR_PURIFIER", "공기청정기"}, {"ENTRY_DOOR", "현관문"},
      {"BATHROOM_DOOR", "화장실문"}, {"VENTILATION_FAN", "환기팬"},
      {"AIR_CONDITIONER", "에어컨"}, {"LAPTOP", "노트북"},
      {"ROBOT_VACUUM", "로봇청소기"}, {"NOTIFICATION", "알림"},
  };
  for (const auto& [name, label] : labels) {
    if (name == device) return label;
  }
  return device;
}

const Words kUnsupportedDeviceAliases = {
    "오븐", "전자레인지", "가스레인지", "보일러",
    // Household appliances HomeMind does not control (registry v3 has none of them).
    "냉장고", "식기세척기", "건조기", "정수기", "전기장판", "인덕션", "밥솥", "커피머신", "커피 머신", "스타일러",
    "가습기", "제습기", "히터", "선풍기",
};

const Words kActionMarkers = {
    "켜", "틀어", "돌려", "키워", "끄", "꺼", "열", "닫", "줄", "낮", "올", "높", "음소거", "상태", "끝났", "조용",
    "미뤄", "재개", "시작", "돌아", "설정", "맞춰", "온도", "저소음", "안 나게", "다시 나게", "다시 알려", "나중에 알려",
};

const Words kStateWords = {"상태", "켜져", "켜졌", "꺼져", "꺼졌", "열려", "열렸", "닫혀", "닫혔", "끝났",
                           "돌았", "다 됐", "몇 도", "몇 단", "돌아가고 있"};
const Words kRelativeDown = {"줄여", "낮춰", "내려"};
const Words kRelativeUp = {"올려", "높여", "키워", "크게"};
const Words kInjectionMarkers = {
    "제한 없",     "권한으로 명령", "확인 같은 거 하지 말", "확인하지 말고", "이전 규칙", "규칙을 무시",
    "규칙은 취소", "지시를 무시",   "json 규칙",            "json 밖",       "시스템 프롬프트", "프롬프트",
    "개발자 모드", "관리자 모드",   "디버그 모드",          "탈옥",
};
const Words kHighRiskMarkers = {
    "불이 났", "불난", "화재", "탄내", "타는 냄새", "연기가 나", "연기 나", "가스 냄새", "가스가 새",
    "침입자", "도둑", "따고 들어", "들어오려", "사람을 다치", "쓰러졌", "숨을 못",
    // medical emergency
    "숨이 안 쉬어", "숨이 잘 안 쉬어", "숨쉬기가 힘", "숨쉬기 힘", "숨이 막", "가슴이 아파", "가슴이 너무 아파",
    "가슴이 조여", "의식이 없", "피가 많이", "구급차", "응급",
};

// Python regexes, verbatim (pyregex maps \s, \S, \d to Python's Unicode classes).
struct Patterns {
  // Command verbs, excluding state forms such as 켜진/켜져/꺼진/열려/닫혀.
  Regex on_verb{"(?:켜|틀어|돌려)(?![져진졌있])"};
  Regex off_verb{"(?:꺼|끄)(?![져진졌])"};
  Regex open_verb{"열(?![려렸린])"};
  Regex close_verb{"닫(?![혀혔힌])"};
  Regex question_end{R"re((?:\?|(?:있어|있나|있니|있어요|됐어|됐나|돌았어|인가|맞지|맞아)\s*\??)\s*$)re"};
  Regex negation{R"re((?:지\s*마|지\s*말|말아\s*(?:줘|주세요)))re"};
  Regex permission_not_to{R"re((?:안|않아)\s*\S*\s*(?:도\s*(?:돼|된다|되|괜찮)))re"};
  Regex volume{R"re((?:볼륨|소리)[^0-9]{0,12}(\d{1,3}))re"};
  Regex absolute_volume{R"re(\d{1,3}\s*(?:으로|로))re"};
  Regex brightness{R"re(밝기[^0-9]{0,12}(\d{1,3}))re"};
  Regex brightness_word{R"re(밝기\s*(백|오십|삼십|이십|십)(?:으로|로)?)re"};
  Regex temperature{R"re((\d{1,2})\s*도)re"};
  Regex level{R"re(([1-5])\s*단(?:계)?)re"};
  Regex planning{R"re((?:할까|할래|할 예정)(?:\s|[.!?]|$))re"};
  Regex explicit_control{
      R"re((?:켜|틀어|꺼|끄|열어|닫아|줄여|올려|낮춰|높여|음소거|설정해|맞춰)(?:\s*(?:줘|주세요|줄래|줄 수|줬으면)))re"};
  Regex bypass{R"re(건너뛰|생략하고|우회|없이\s*(?:바로|그냥|전부|다)|무시하고)re"};
  Regex break_in{R"re(문\s*(?:을|를)?\s*(?:계속\s*|자꾸\s*|몰래\s*)?따(?:려|고|는|네))re"};
};

const Patterns& patterns() {
  static const Patterns compiled;
  return compiled;
}

bool has(std::string_view text, std::string_view word) { return text.find(word) != std::string_view::npos; }

bool has_any(std::string_view text, const Words& words) {
  return std::any_of(words.begin(), words.end(), [&](std::string_view word) { return has(text, word); });
}

// int() of a run of Unicode decimal digits (\d matched them).
int digits_value(std::string_view digits) {
  int value = 0;
  for (char32_t c : text::decode(digits)) value = value * 10 + std::max(0, text::decimal_value(c));
  return value;
}

ojson proposal(const char* intent, const char* device = nullptr, const char* action = nullptr,
               ojson parameters = ojson::object(), bool ambiguous = false, std::vector<const char*> flags = {},
               const std::optional<std::string>& question = std::nullopt) {
  ojson result = ojson::object();
  result["intent"] = intent;
  result["device"] = device ? ojson(device) : ojson(nullptr);
  result["action"] = action ? ojson(action) : ojson(nullptr);
  result["parameters"] = std::move(parameters);
  result["ambiguous"] = ambiguous;
  result["safety_flags"] = ojson::array();
  for (const char* flag : flags) result["safety_flags"].push_back(flag);
  result["clarification_question"] = question ? ojson(*question) : ojson(nullptr);
  return result;
}

ojson clarify(const char* flag, const std::string& question) {
  return proposal("CLARIFY", nullptr, nullptr, ojson::object(), true, {flag}, question);
}

ojson single_parameter(const char* name, int value) {
  ojson parameters = ojson::object();
  parameters[name] = value;
  return parameters;
}

std::ptrdiff_t last_match(const Regex& pattern, std::string_view text) {
  const auto starts = pattern.finditer_starts(text);
  return starts.empty() ? -1 : static_cast<std::ptrdiff_t>(starts.back());
}

// The action whose verb appears last in the sentence (the final verb wins).
const char* last_of(std::string_view text, const char* first, const Regex& first_pattern, const char* second,
                    const Regex& second_pattern) {
  const auto first_at = last_match(first_pattern, text);
  const auto second_at = last_match(second_pattern, text);
  if (first_at < 0 && second_at < 0) return nullptr;
  return first_at > second_at ? first : second;
}

bool is_injection(std::string_view text) {
  if (has_any(text, kInjectionMarkers)) return true;
  const bool names_rules = has_any(text, {"규칙", "지시", "명령", "설정"});
  const bool overrides = has_any(text, {"무시", "잊", "취소하고", "해제하고", "없던 걸로"});
  if (names_rules && overrides) return true;
  // Asking to skip the safety checks themselves ("정책 검사 건너뛰고", "승인 없이 바로").
  const bool names_checks = has_any(text, {"검사", "검증", "확인", "승인", "정책", "보안"});
  const bool bypass = patterns().bypass.search(text).has_value();
  return names_checks && bypass;
}

}  // namespace

ojson preflight_proposal(std::string_view utterance, const ojson& capabilities, const ojson& context) {
  const Patterns& re = patterns();
  // utterance.casefold().strip(); lone surrogates become U+FFFD (same byte length) so PCRE2 accepts the subject.
  const std::string text = text::sanitize(text::strip(text::casefold(text::sanitize(utterance))));

  std::vector<std::string_view> devices;
  for (const auto& [device, aliases] : device_aliases()) {
    if (has_any(text, aliases)) devices.push_back(device);
  }
  auto mentions = [&](std::string_view device) {
    return std::find(devices.begin(), devices.end(), device) != devices.end();
  };
  // "세탁기 알림" names the washer's notification feature, not two independently targeted devices.
  if (mentions("WASHER") && mentions("NOTIFICATION")) {
    devices.erase(std::find(devices.begin(), devices.end(), std::string_view("NOTIFICATION")));
  }
  std::set<std::string, std::less<>> enabled;
  if (capabilities.is_object() && capabilities.contains("devices") && capabilities["devices"].is_object()) {
    for (const auto& [name, value] : capabilities["devices"].items()) {
      if (value.is_object() && value.contains("enabled") && py::truthy(value["enabled"])) enabled.insert(name);
    }
  }
  bool quiet_required = false;
  if (context.is_object() && context.contains("quiet_required")) quiet_required = py::truthy(context["quiet_required"]);

  if (is_injection(text)) {
    return clarify("CONFLICTING_REQUEST", "요청에 상충하는 지시가 있습니다. 원하는 기기 동작만 다시 말해 주세요.");
  }

  // Negation in any ending and "안 해도 돼" style permission not to act must never become a command.
  if (re.negation.search(text) || re.permission_not_to.search(text)) {
    return proposal("NO_ACTION", nullptr, nullptr, ojson::object(), false, {"NEGATED_REQUEST"});
  }

  if (has_any(text, kHighRiskMarkers) || re.break_in.search(text)) {
    return clarify("HIGH_RISK", "위험 상황은 자동 실행하지 않습니다. 안전을 확인한 뒤 기기 동작을 다시 말해 주세요.");
  }

  std::optional<std::string_view> unsupported_name;
  for (std::string_view name : kUnsupportedDeviceAliases) {
    if (has(text, name)) {
      unsupported_name = name;
      break;
    }
  }
  std::optional<std::string_view> disabled;
  for (std::string_view device : devices) {
    if (!enabled.count(device)) {
      disabled = device;
      break;
    }
  }
  if (unsupported_name || disabled) {
    const std::string label(unsupported_name ? *unsupported_name : device_label(*disabled));
    return clarify("UNSUPPORTED_DEVICE", label + "은(는) 현재 지원되지 않습니다. 다른 기기를 말씀하셨나요?");
  }

  // A quiet context must not swallow an explicit device command spoken during a meeting;
  // it only answers sentences that name no device.
  if (quiet_required && devices.empty() && has_any(text, {"조용", "소리", "방해", "시끄", "알림"})) {
    return proposal("CONTEXT_SUGGESTION", "ALL_QUIET_DEVICES", "SET_QUIET_MODE");
  }

  if (devices.empty() &&
      has_any(text, {"그거", "이거", "저거", "그 장치", "이 장치", "저 장치", "그 기기", "이 기기", "저 기기"})) {
    return clarify("AMBIGUOUS_TARGET", "어느 기기를 말씀하셨나요?");
  }

  const auto volume = re.volume.search(text);
  const auto absolute_volume = re.absolute_volume.search(text);
  if (mentions("TV") && volume && !absolute_volume) {
    // "소리 10 정도 줄여줘" is a relative change, not "set the volume to 10".
    if (has_any(text, kRelativeDown)) return proposal("DEVICE_CONTROL", "TV", "DECREASE_VOLUME");
    if (has_any(text, kRelativeUp)) return proposal("DEVICE_CONTROL", "TV", "INCREASE_VOLUME");
  }
  if (mentions("TV") && volume) {
    const int value = digits_value(volume->group(text, 1));
    if (value > 100) return clarify("AMBIGUOUS_ACTION", "TV 볼륨은 0부터 100 사이로 말해 주세요.");
    return proposal("DEVICE_CONTROL", "TV", "SET_VOLUME", single_parameter("volume", value));
  }

  const auto brightness = re.brightness.search(text);
  const auto brightness_word = re.brightness_word.search(text);
  if (mentions("LIGHT") && brightness) {
    const int value = digits_value(brightness->group(text, 1));
    if (value > 100) return clarify("AMBIGUOUS_ACTION", "조명 밝기는 0부터 100 사이로 말해 주세요.");
    return proposal("DEVICE_CONTROL", "LIGHT", "SET_BRIGHTNESS", single_parameter("brightness", value));
  }
  if (mentions("LIGHT") && brightness_word) {
    const std::string word = brightness_word->group(text, 1);
    const int value = word == "백" ? 100 : word == "오십" ? 50 : word == "삼십" ? 30 : word == "이십" ? 20 : 10;
    return proposal("DEVICE_CONTROL", "LIGHT", "SET_BRIGHTNESS", single_parameter("brightness", value));
  }

  // Whole-room quiet context outranks a single notification alias in the same sentence.
  const bool quiet_request =
      (has_any(text, {"조용", "방해되는 소리", "전체 소리"}) && has_any(text, {"바꿔", "줄여", "멈춰", "없애"})) ||
      (has(text, "회의") && has(text, "알림") && has_any(text, {"줄여", "조용", "미뤄"}));
  if (quiet_request) return proposal("CONTEXT_SUGGESTION", "ALL_QUIET_DEVICES", "SET_QUIET_MODE");

  if (devices.size() == 1) {
    const std::string device(devices.front());
    const char* d = device.c_str();

    const bool has_command_verb = re.on_verb.search(text) || re.off_verb.search(text) || re.open_verb.search(text) ||
                                  re.close_verb.search(text);
    const bool asks_state = has_any(text, kStateWords) || re.question_end.search(text).has_value();
    if (asks_state && !has_command_verb) return proposal("DEVICE_QUERY", d, "QUERY_STATUS");

    if (device == "LIGHT" && has(text, "밝혀")) return proposal("DEVICE_CONTROL", d, "TURN_ON");

    if (device == "TV") {
      if (has_any(text, {"다시 나게", "다시 나오게", "음소거 해제", "음소거를 풀", "음소거 풀"})) {
        return proposal("DEVICE_CONTROL", d, "UNMUTE");
      }
      if (has_any(text, {"안 나게", "음소거"})) return proposal("DEVICE_CONTROL", d, "MUTE");
      if (has_any(text, {"줄여", "낮춰", "내려", "작게"})) return proposal("DEVICE_CONTROL", d, "DECREASE_VOLUME");
      if (has_any(text, {"올려", "높여", "키워", "크게"})) return proposal("DEVICE_CONTROL", d, "INCREASE_VOLUME");
    }

    if (device == "WASHER") {
      if (has_any(text, {"다시 알려", "재개", "다시 받", "다시 보내"})) {
        return proposal("DEVICE_CONTROL", d, "RESUME_NOTIFICATION");
      }
      if (has(text, "나중에 알려") || has(text, "미뤄")) return proposal("DEVICE_CONTROL", d, "DEFER_NOTIFICATION");
    }

    if (device == "AIR_CONDITIONER" && has_any(text, {"저소음", "조용한 모드", "조용하게"})) {
      return proposal("DEVICE_CONTROL", d, "SET_QUIET_MODE");
    }

    if (device == "AIR_CONDITIONER") {
      if (const auto temperature = re.temperature.search(text)) {
        const int value = digits_value(temperature->group(text, 1));
        if (value < 16 || value > 30) {
          return clarify("AMBIGUOUS_ACTION", "에어컨 온도는 16도부터 30도 사이로 말해 주세요.");
        }
        return proposal("DEVICE_CONTROL", d, "SET_TEMPERATURE", single_parameter("temperature_celsius", value));
      }
      if (const auto level = re.level.search(text)) {
        return proposal("DEVICE_CONTROL", d, "SET_LEVEL", single_parameter("level", digits_value(level->group(text, 1))));
      }
    }

    if (device == "NOTIFICATION") {
      if (has_any(text, {"재개", "다시 시작", "다시 보내", "다시 받", "다시 알려"})) {
        return proposal("DEVICE_CONTROL", d, "RESUME_NOTIFICATION");
      }
      if (has_any(text, {"나중", "미뤄"})) return proposal("DEVICE_CONTROL", d, "DEFER_NOTIFICATION");
      if (has_any(text, {"음소거를 풀", "음소거 풀", "음소거 해제", "다시 들리", "다시 나"})) {
        return proposal("DEVICE_CONTROL", d, "UNMUTE");
      }
      if (has_any(text, {"음소거", "소리 없애"})) return proposal("DEVICE_CONTROL", d, "MUTE");
    }

    if (device == "ENTRY_DOOR" || device == "BATHROOM_DOOR" || device == "WINDOW") {
      if (const char* action = last_of(text, "OPEN", re.open_verb, "CLOSE", re.close_verb)) {
        return proposal("DEVICE_CONTROL", d, action);
      }
    }

    if (const char* action = last_of(text, "TURN_ON", re.on_verb, "TURN_OFF", re.off_verb)) {
      return proposal("DEVICE_CONTROL", d, action);
    }
  }

  if (!devices.empty() && re.planning.search(text)) return proposal("NO_ACTION");

  if (!devices.empty() && !has_any(text, kActionMarkers)) {
    return clarify("AMBIGUOUS_ACTION", "해당 기기를 어떻게 할까요?");
  }

  const bool contextual = has_any(text, {"회의", "집중", "조용"});
  if (has(text, "회의 준비") && has_any(text, {"다 했", "끝났", "됐"})) return proposal("NO_ACTION");
  if (devices.empty() && !contextual) {
    if (re.explicit_control.search(text)) return clarify("AMBIGUOUS_TARGET", "어느 기기를 말씀하셨나요?");
    // No device and no explicit command: a possible hidden intent ("좀 덥네"). EXAONE decides; its proposal
    // still needs UI approval (task-policy llm_needs_approval).
    return nullptr;
  }
  return nullptr;
}

}  // namespace hm::guard
