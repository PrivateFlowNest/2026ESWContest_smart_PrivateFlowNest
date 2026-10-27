// JSONL probe for the PC parity tests (native/tests/test_guard_parity.py). One request per stdin line,
// one ASCII-only JSON line out. Requests are parsed with Python json semantics (key order kept).
//   {"op":"preflight","utterance":s,"context":c}             -> {"proposal": p|null}
//   {"op":"validate_proposal","proposal":v}                  -> {"ok": p} | {"errors": [...]} | {"type_error": s}
//   {"op":"validate_activity_context","context":c}          -> same shape
//   {"op":"payload","utterance":s,"context":c,"feedback":[..],"candidate":p,"model":m,...} -> {"body": s}
//   {"op":"interpret","utterance":s,"context":c,"base_url":u,...} -> InterpretationResult.as_dict(include_raw=True)
//   {"op":"loads","text":s} -> {"value": v, "dumps": s} | {"decode_error": msg, "what": s}
//   {"op":"text","text":s} -> casefold/strip/repr/length;  {"op":"repr","value":v};  {"op":"float_repr","value":x}
//   {"op":"utf8_error","hex":h};  {"op":"char_table","start":a,"end":b};  {"op":"regex","pattern":p,"text":s}
//   {"op":"uuid","value":s};  {"op":"llm_request_check","request":r}
#include <functional>
#include <iostream>
#include <string>

#include "hm/config.hpp"
#include "hm/exaone_client.hpp"
#include "hm/guardrails.hpp"
#include "hm/pfn_schema.hpp"
#include "hm/pyregex.hpp"
#include "hm/text.hpp"
#include "hm/validation.hpp"

namespace {

using hm::py::ojson;

const ojson& get(const ojson& line, const char* key) {
  static const ojson null_value = nullptr;
  auto it = line.find(key);
  return it == line.end() ? null_value : *it;
}

std::size_t cp_offset(const std::string& subject, std::size_t byte_offset) {
  return hm::text::length(std::string_view(subject).substr(0, byte_offset));
}

ojson validation_outcome(const std::function<ojson()>& run) {
  ojson out = ojson::object();
  try {
    out["ok"] = run();
  } catch (const hm::guard::ProposalValidationError& error) {
    out["errors"] = ojson::array();
    for (const auto& message : error.errors()) out["errors"].push_back(message);
  } catch (const hm::guard::PyTypeError& error) {
    out["type_error"] = error.what();
  }
  return out;
}

ojson handle(const ojson& line, const ojson& capabilities, const ojson& schema, const ojson& context_schema) {
  const std::string op = line.at("op").get<std::string>();
  ojson out = ojson::object();
  if (op == "preflight") {
    const ojson& caps = line.contains("capabilities") ? line["capabilities"] : capabilities;
    out["proposal"] = hm::guard::preflight_proposal(line.at("utterance").get<std::string>(), caps, get(line, "context"));
    return out;
  }
  if (op == "validate_proposal") {
    return validation_outcome([&] { return hm::guard::validate_proposal(get(line, "proposal"), schema, capabilities); });
  }
  if (op == "validate_activity_context") {
    return validation_outcome([&] { return hm::guard::validate_activity_context(get(line, "context"), context_schema); });
  }
  if (op == "payload" || op == "interpret") {
    hm::guard::ExaoneClientOptions options;
    options.base_url = line.value("base_url", std::string("http://127.0.0.1:8080"));
    options.model = line.value("model", std::string("exaone-4.0-1.2b"));
    options.schema = schema;
    options.capabilities = line.contains("capabilities") ? line["capabilities"] : capabilities;
    options.activity_context_schema = context_schema;
    options.timeout_seconds = line.value("timeout_seconds", 120.0);
    options.max_tokens = line.value("max_tokens", 256);
    options.retries = line.value("retries", 1);
    options.enable_guardrails = line.value("enable_guardrails", true);
    options.include_few_shots = line.value("include_few_shots", true);
    hm::guard::ExaoneClient client(options);
    if (op == "payload") {
      std::vector<std::string> feedback;
      for (const auto& item : get(line, "feedback")) feedback.push_back(item.get<std::string>());
      out["body"] = hm::py::dumps(client.payload(line.at("utterance").get<std::string>(), get(line, "context"),
                                                 feedback, get(line, "candidate")));
      return out;
    }
    try {
      return client.interpret(line.at("utterance").get<std::string>(), get(line, "context")).as_dict(true);
    } catch (const hm::guard::ProposalValidationError& error) {
      out["exception"] = "ProposalValidationError";
      out["message"] = error.what();
    } catch (const std::invalid_argument& error) {
      out["exception"] = "ValueError";
      out["message"] = error.what();
    }
    return out;
  }
  if (op == "loads") {
    try {
      const ojson value = hm::py::loads(line.at("text").get<std::string>());
      out["value"] = value;
      out["dumps"] = hm::py::dumps(value);
      out["repr"] = hm::py::repr(value);
    } catch (const hm::py::JsonDecodeError& error) {
      out["decode_error"] = error.msg();
      out["what"] = error.what();
    } catch (const hm::py::RecursionError& error) {
      out["recursion_error"] = error.what();
    } catch (const hm::py::ValueError& error) {
      out["value_error"] = error.what();
    }
    return out;
  }
  if (op == "text") {
    const std::string text = line.at("text").get<std::string>();
    out["casefold"] = hm::text::casefold(text);
    out["strip"] = hm::text::strip(text);
    out["repr"] = hm::text::repr(text);
    out["length"] = hm::text::length(text);
    return out;
  }
  if (op == "repr") {
    out["repr"] = hm::py::repr(get(line, "value"));
    out["truthy"] = hm::py::truthy(get(line, "value"));
    return out;
  }
  if (op == "float_repr") {
    out["repr"] = hm::text::float_repr(line.at("value").get<double>());
    return out;
  }
  if (op == "utf8_error") {
    const std::string hex = line.at("hex").get<std::string>();
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    const auto error = hm::text::utf8_decode_error(bytes);
    out["error"] = error ? ojson(*error) : ojson(nullptr);
    return out;
  }
  if (op == "char_table") {
    // Per code point in [start, end): casefold changes, and ranges of non-space/printable/decimal classes.
    const auto start = line.at("start").get<std::uint32_t>();
    const auto end = line.at("end").get<std::uint32_t>();
    ojson folds = ojson::array();
    ojson spaces = ojson::array();
    ojson nonprintable = ojson::array();
    ojson decimals = ojson::array();
    auto add_range = [](ojson& ranges, std::uint32_t cp) {
      if (!ranges.empty() && ranges.back()[1].get<std::uint32_t>() == cp) {
        ranges.back()[1] = cp + 1;
      } else {
        ranges.push_back(ojson::array({cp, cp + 1}));
      }
    };
    for (std::uint32_t cp = start; cp < end; ++cp) {
      std::string one;
      hm::text::append_utf8(one, cp);
      if (cp < 0xD800 || cp > 0xDFFF) {
        const std::string folded = hm::text::casefold(one);
        if (folded != one) folds.push_back(ojson::array({cp, folded}));
      }
      if (hm::text::is_space(cp)) add_range(spaces, cp);
      if (!hm::text::is_printable(cp)) add_range(nonprintable, cp);
      const int digit = hm::text::decimal_value(cp);
      if (digit >= 0) decimals.push_back(ojson::array({cp, digit}));
    }
    out["folds"] = folds;
    out["spaces"] = spaces;
    out["nonprintable"] = nonprintable;
    out["decimals"] = decimals;
    return out;
  }
  if (op == "regex") {
    const std::string subject = line.at("text").get<std::string>();
    hm::py::Regex regex(line.at("pattern").get<std::string>());
    const auto match = regex.search(subject);
    if (!match) {
      out["match"] = nullptr;
      return out;
    }
    ojson groups = ojson::array();
    for (const auto& [b, e] : match->groups) {
      if (b == std::string::npos) {
        groups.push_back(nullptr);
      } else {
        groups.push_back(ojson::array({cp_offset(subject, b), cp_offset(subject, e)}));
      }
    }
    out["match"] = groups;
    return out;
  }
  if (op == "uuid") {
    out["valid"] = hm::guard::jsonschema_uuid(line.at("value").get<std::string>());
    return out;
  }
  if (op == "llm_request_check") {
    out["problem"] = hm::guard::validate_llm_request(hm::py::to_plain(get(line, "request")));
    return out;
  }
  throw std::runtime_error("unknown op " + op);
}

}  // namespace

int main() {
  const ojson capabilities = hm::guard::load_json_object(hm::guard::default_capabilities_path());
  const ojson schema = hm::guard::load_json_object(hm::guard::default_schema_path());
  const ojson context_schema = hm::guard::load_json_object(hm::guard::default_activity_context_schema_path());
  std::string text;
  while (std::getline(std::cin, text)) {
    if (text.empty()) continue;
    ojson answer;
    try {
      answer = handle(hm::py::loads(text), capabilities, schema, context_schema);
    } catch (const std::exception& error) {
      answer = ojson::object();
      answer["error"] = error.what();
    }
    std::cout << hm::py::dumps(answer, true, true) << '\n' << std::flush;
  }
  return 0;
}
