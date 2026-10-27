// Minimal blocking HTTP/1.1 client for the local model servers (llama-server, whisper-server).
// Plain http:// only (both servers listen on 127.0.0.1); no TLS, no redirects, no proxies.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hm::http {

struct Response {
  int status = 0;  // 0 = transport error (see error)
  std::string body;
  std::map<std::string, std::string> headers;  // lowercase names
  std::string error;
  bool ok() const { return status >= 200 && status < 300; }
};

struct Url {
  std::string host;
  int port = 80;
  std::string path = "/";
};
// "http://127.0.0.1:8080/v1/chat" -> {127.0.0.1, 8080, /v1/chat}. Throws std::invalid_argument.
Url parse_url(const std::string& url);

Response request(const std::string& method, const std::string& url, const std::string& body,
                 const std::string& content_type, int timeout_ms);
inline Response get(const std::string& url, int timeout_ms) { return request("GET", url, "", "", timeout_ms); }
inline Response post_json(const std::string& url, const std::string& body, int timeout_ms) {
  return request("POST", url, body, "application/json", timeout_ms);
}

struct FormPart {
  std::string name;
  std::string value;
  std::string filename;      // non-empty -> file part
  std::string content_type;  // for file parts
};
// multipart/form-data POST (whisper-server /inference).
Response post_multipart(const std::string& url, const std::vector<FormPart>& parts, int timeout_ms);

}  // namespace hm::http
