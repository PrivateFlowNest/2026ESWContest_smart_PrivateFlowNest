#include "api.hpp"

#include <algorithm>
#include <cctype>

namespace hm::mobile {

namespace {

HttpResponse detail(int status, const std::string& message) { return {status, {{"detail", message}}, {}}; }

HttpResponse unauthorized(const std::string& message, const std::string& challenge) {
  HttpResponse response = detail(401, message);
  response.headers.emplace_back("WWW-Authenticate", challenge);
  return response;
}

HttpResponse method_not_allowed(const char* allowed) {
  HttpResponse response = detail(405, "Method Not Allowed");
  response.headers.emplace_back("Allow", allowed);
  return response;
}

std::string strip_ascii(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n\f\v");
  if (begin == std::string::npos) return {};
  const auto end = text.find_last_not_of(" \t\r\n\f\v");
  return text.substr(begin, end - begin + 1);
}

std::string upper_ascii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
  return text;
}

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> out;
  std::size_t start = 1;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    out.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return out;
}

// Request body model field (pydantic str field).
struct Field {
  const char* name;
  bool required;
  const char* fallback;
  std::size_t max_length;  // 0 = unlimited
};

// FastAPI body handling: JSON only with an application/json (or +json) content type, then pydantic validation.
// Returns the validated values, or fills `error` with the 422 response.
bool validate_body(const HttpRequest& request, const std::vector<Field>& fields, json& values, HttpResponse& error) {
  auto fail = [&](json errors) {
    error = {422, {{"detail", std::move(errors)}}, {}};
    return false;
  };
  json body = nullptr;
  if (!request.body.empty()) {
    std::string type = request.content_type.substr(0, request.content_type.find(';'));
    type = upper_ascii(strip_ascii(type));
    const bool is_json = type == "APPLICATION/JSON" ||
                         (type.rfind("APPLICATION/", 0) == 0 && type.size() > 17 &&
                          type.compare(type.size() - 5, 5, "+JSON") == 0);
    if (is_json) {
      try {
        body = json::parse(request.body);
      } catch (const json::parse_error& parse_error) {
        return fail(json::array({{{"type", "json_invalid"},
                                  {"loc", {"body", parse_error.byte > 0 ? parse_error.byte - 1 : 0}},
                                  {"msg", "JSON decode error"},
                                  {"input", json::object()},
                                  {"ctx", {{"error", "invalid JSON"}}}}}));
      }
    } else {
      body = request.body;  // bytes: not a model
    }
  }
  if (body.is_null()) {
    return fail(json::array({{{"type", "missing"}, {"loc", {"body"}}, {"msg", "Field required"}, {"input", nullptr}}}));
  }
  if (!body.is_object()) {
    return fail(json::array({{{"type", "model_attributes_type"},
                              {"loc", {"body"}},
                              {"msg", "Input should be a valid dictionary or object to extract fields from"},
                              {"input", body}}}));
  }
  json errors = json::array();
  values = json::object();
  for (const auto& field : fields) {
    auto it = body.find(field.name);
    if (it == body.end()) {
      if (field.required) {
        errors.push_back({{"type", "missing"}, {"loc", {"body", field.name}}, {"msg", "Field required"}, {"input", body}});
      } else {
        values[field.name] = field.fallback;
      }
      continue;
    }
    if (!it->is_string()) {
      errors.push_back({{"type", "string_type"},
                        {"loc", {"body", field.name}},
                        {"msg", "Input should be a valid string"},
                        {"input", *it}});
      continue;
    }
    const std::string& value = it->get_ref<const std::string&>();
    if (field.max_length && utf8_length(value) > field.max_length) {
      errors.push_back({{"type", "string_too_long"},
                        {"loc", {"body", field.name}},
                        {"msg", "String should have at most " + std::to_string(field.max_length) + " characters"},
                        {"input", value},
                        {"ctx", {{"max_length", field.max_length}}}});
      continue;
    }
    values[field.name] = value;
  }
  if (!errors.empty()) return fail(std::move(errors));
  return true;
}

}  // namespace

std::size_t utf8_length(const std::string& text) {
  return static_cast<std::size_t>(
      std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

bool requires_token(const std::string& method, const std::string& path) {
  if (method == "OPTIONS") return false;  // CORS preflight carries no credentials
  if (path.rfind("/api/", 0) != 0) return false;
  return path != "/api/v1/health" && path != "/api/v1/pairing/claim";
}

Api::Api(const Registry& registry, HardwareStore& store, CoreBridge& bridge, TokenStore& tokens,
         PairingService& pairing, ApprovalBook& approvals)
    : registry_(registry), store_(store), bridge_(bridge), tokens_(tokens), pairing_(pairing), approvals_(approvals) {}

bool Api::authorized(const std::string& authorization_header) const {
  const auto token = bearer_token(authorization_header);
  return token && tokens_.verify(*token);
}

HttpResponse Api::handle(const HttpRequest& request) {
  try {
    if (requires_token(request.method, request.path)) {
      const auto token = bearer_token(request.authorization);
      if (!token) return unauthorized("missing bearer token", "Bearer");
      if (!tokens_.verify(*token)) {
        log(LogLevel::info, "auth", "refused token from " + request.remote_addr + " for " + request.path);
        return unauthorized("invalid or revoked token", "Bearer error=\"invalid_token\"");
      }
    }
    return route(request);
  } catch (const std::exception& error) {
    log(LogLevel::error, "api", request.method + " " + request.path + " failed: " + error.what());
    return detail(500, "Internal Server Error");
  }
}

HttpResponse Api::route(const HttpRequest& request) {
  const std::string& path = request.path;
  const bool get = request.method == "GET";
  const bool post = request.method == "POST";
  if (path == "/") {
    if (!get) return method_not_allowed("GET");
    return {200,
            {{"service", "HomeMind Mobile API"},
             {"hardware_map", "/api/v1/hardware-map"},
             {"devices", "/api/v1/devices"}},
            {}};
  }
  if (path == "/api/v1/health") return get ? health() : method_not_allowed("GET");
  if (path == "/api/v1/dashboard") {
    if (!get) return method_not_allowed("GET");
    return {200, {{"snapshot", store_.snapshot(bridge_.connected())}}, {}};
  }
  if (path == "/api/v1/hardware-map") return get ? HttpResponse{200, store_.hardware_map(), {}} : method_not_allowed("GET");
  if (path == "/api/v1/devices") {
    if (!get) return method_not_allowed("GET");
    return {200, {{"devices", store_.devices()}}, {}};
  }
  if (path == "/api/v1/pairing/claim") return post ? claim(request) : method_not_allowed("POST");
  if (path == "/api/v1/approvals") return get ? approvals() : method_not_allowed("GET");
  const auto segments = split_path(path);
  if (segments.size() == 5 && segments[0] == "api" && segments[1] == "v1" && segments[2] == "tasks" &&
      !segments[3].empty() && (segments[4] == "approve" || segments[4] == "reject")) {
    return post ? decide(segments[3], segments[4]) : method_not_allowed("POST");
  }
  if (segments.size() == 5 && segments[0] == "api" && segments[1] == "v1" && segments[2] == "devices" &&
      !segments[3].empty() && segments[4] == "commands") {
    return post ? command(segments[3], request) : method_not_allowed("POST");
  }
  if (segments.size() == 4 && segments[0] == "api" && segments[1] == "v1" && segments[2] == "tasks" &&
      !segments[3].empty()) {
    return get ? task_status(segments[3]) : method_not_allowed("GET");
  }
  return detail(404, "Not Found");
}

HttpResponse Api::health() const {
  return {200,
          {{"status", "ok"},
           {"service", kServiceName},
           {"version", kVersion},
           {"mqtt_enabled", bridge_.enabled()},
           {"mqtt_connected", bridge_.connected()},
           {"command_path", "pfn/v1/core/task/request (core)"}},
          {}};
}

HttpResponse Api::command(const std::string& device_id, const HttpRequest& request) {
  json values;
  HttpResponse error;
  if (!validate_body(request, {{"command", true, "", 0}}, values, error)) return error;
  if (!store_.device(device_id)) return detail(404, "device ID is not in device-registry.json");
  const std::string command = upper_ascii(strip_ascii(values["command"].get<std::string>()));
  ControlRequest control;
  try {
    control = registry_.control(device_id, command);
  } catch (const UnsupportedCommand& unsupported) {
    return detail(400, unsupported.what());
  }
  const auto task = bridge_.request(control, command);
  if (!task) return detail(503, "core MQTT connection is not available");
  const DeviceView device = store_.mark_pending(device_id, command, task->task_id);
  const bool needs_approval = control.device_id == "SERVO-DOOR-01" && control.position_ref == "open";
  const std::string message = needs_approval
                                  ? "현관문 열기는 7인치 화면에서 승인해야 실행됩니다."
                                  : device.name + " " + command + " 요청을 core에 보냈습니다. 장치 응답(ACK) 후 상태가 바뀝니다.";
  return {202,
          {{"device", device.to_json()},
           {"accepted", true},
           {"published", true},
           {"message", message},
           {"task_id", task->task_id},
           {"task_state", task->state}},
          {}};
}

HttpResponse Api::approvals() const {
  json list = json::array();
  for (const auto& approval : approvals_.pending()) {
    const bool allowed = phone_may_approve(approval);
    list.push_back({{"task_id", approval.task_id},
                    {"title", approval_title(approval, registry_)},
                    {"intent", approval.intent},
                    {"target_device", approval.target_device},
                    {"origin", approval.origin},
                    {"phone_can_approve", allowed},
                    {"note", allowed ? json(nullptr) : json("현관문 열기는 집 안 7인치 화면에서 승인해요")},
                    {"waiting_since_ms", approval.seen_ms}});
  }
  return {200, {{"approvals", list}}, {}};
}

HttpResponse Api::decide(const std::string& task_id, const std::string& action) {
  const auto approval = approvals_.find(task_id);
  if (!approval) return detail(404, "no approval is waiting for this task");
  if (action == "approve" && !phone_may_approve(*approval)) {
    return detail(403, "opening the entrance door is approved on the 7-inch screen in the house");
  }
  if (!bridge_.connected()) return detail(503, "core is not reachable (MQTT)");
  if (!bridge_.send_task_action(ui_action_document(*approval, action, bridge_.config().client_id, now_ms()))) {
    return detail(503, "core is not reachable (MQTT)");
  }
  log(LogLevel::info, "api", "phone " + action + " for task " + task_id.substr(0, 8));
  return {202, {{"task_id", task_id}, {"action", action}, {"status", "sent"}}, {}};
}

HttpResponse Api::task_status(const std::string& task_id) const {
  const auto task = bridge_.task(task_id);
  if (!task) {
    // a task this phone did not create (an approval it answered): last state core reported
    const auto state = approvals_.state_of(task_id);
    if (!state) return detail(404, "unknown or expired task");
    return {200,
            {{"task_id", task_id},
             {"device_id", nullptr},
             {"command", nullptr},
             {"state", *state},
             {"failure_code", nullptr},
             {"failure_message", nullptr}},
            {}};
  }
  const auto code = task->failure_field("code");
  const auto message = task->failure_field("message");
  return {200,
          {{"task_id", task->task_id},
           {"device_id", task->device_id},
           {"command", task->command},
           {"state", task->state},
           {"failure_code", code ? json(*code) : json(nullptr)},
           {"failure_message", message ? json(*message) : json(nullptr)}},
          {}};
}

HttpResponse Api::claim(const HttpRequest& request) {
  json values;
  HttpResponse error;
  if (!validate_body(request, {{"code", true, "", 32}, {"nonce", true, "", 128}, {"device_name", false, "", 64}},
                     values, error)) {
    return error;
  }
  const auto result = pairing_.claim(values["code"].get<std::string>(), values["nonce"].get<std::string>(),
                                     values["device_name"].get<std::string>(), request.remote_addr, now_ms());
  switch (result.status) {
    case PairingService::Status::rate_limited: {
      HttpResponse response = detail(429, "too many failed pairing attempts; try again later");
      response.headers.emplace_back("Retry-After", std::to_string(result.retry_after_s));
      return response;
    }
    case PairingService::Status::invalid:
      return detail(401, "invalid, expired or already used pairing code");
    case PairingService::Status::ok:
      break;
  }
  if (on_paired_) on_paired_();
  return {201,
          {{"token", result.token},
           {"token_id", result.record.id},
           {"token_type", "Bearer"},
           {"device_id", pairing_.config().device_id},
           {"device_name", pairing_.config().device_name}},
          {}};
}

}  // namespace hm::mobile
