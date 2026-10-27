// HTTP API of the Mobile API, independent of the HTTP library (main.cpp adapts cpp-httplib; the unit tests call
// handle() directly). Same endpoints, status codes and JSON as the Python reference (FastAPI, backend/app/main.py),
// including FastAPI's {"detail": ...} errors and 422 validation lists.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "approvals.hpp"
#include "auth.hpp"
#include "core_bridge.hpp"
#include "hm/common.hpp"
#include "store.hpp"

namespace hm::mobile {

inline constexpr const char* kServiceName = "homemind-mobile-api";
inline constexpr const char* kVersion = "0.5.0-beta";

struct HttpRequest {
  std::string method;
  std::string path;  // percent-decoded, without the query
  std::string authorization;
  std::string content_type;
  std::string body;
  std::string remote_addr;
};

struct HttpResponse {
  int status = 200;
  json body = json::object();
  std::vector<std::pair<std::string, std::string>> headers;
};

class Api {
 public:
  Api(const Registry& registry, HardwareStore& store, CoreBridge& bridge, TokenStore& tokens, PairingService& pairing,
      ApprovalBook& approvals);

  HttpResponse handle(const HttpRequest& request);
  // Called after a phone paired (the 7-inch UI phone list is sent again).
  void on_paired(std::function<void()> callback) { on_paired_ = std::move(callback); }
  // Bearer token check for routes served outside handle() (GET /api/v1/events).
  bool authorized(const std::string& authorization_header) const;

 private:
  HttpResponse route(const HttpRequest& request);
  HttpResponse health() const;
  HttpResponse command(const std::string& device_id, const HttpRequest& request);
  HttpResponse task_status(const std::string& task_id) const;
  HttpResponse claim(const HttpRequest& request);
  HttpResponse approvals() const;
  HttpResponse decide(const std::string& task_id, const std::string& action);

  const Registry& registry_;
  HardwareStore& store_;
  CoreBridge& bridge_;
  TokenStore& tokens_;
  PairingService& pairing_;
  ApprovalBook& approvals_;
  std::function<void()> on_paired_;
};

// Paths reachable without a token: everything outside /api/, the health check and the pairing claim.
bool requires_token(const std::string& method, const std::string& path);
// Number of Unicode code points in UTF-8 text (pydantic max_length counts characters).
std::size_t utf8_length(const std::string& text);

}  // namespace hm::mobile
