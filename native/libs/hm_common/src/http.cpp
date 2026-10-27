#include "hm/http.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include "hm/common.hpp"

namespace hm::http {

namespace {

using Clock = std::chrono::steady_clock;

struct Socket {
  int fd = -1;
  ~Socket() {
    if (fd >= 0) ::close(fd);
  }
};

int remaining_ms(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return left > 0 ? static_cast<int>(left) : 0;
}

bool wait_fd(int fd, short events, Clock::time_point deadline) {
  pollfd item{fd, events, 0};
  for (;;) {
    const int rc = ::poll(&item, 1, remaining_ms(deadline));
    if (rc > 0) return true;
    if (rc == 0) return false;
    if (errno != EINTR) return false;
  }
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  return text;
}

// Connect with a deadline (non-blocking connect + poll). Returns -1 with `error` set on failure.
int connect_to(const Url& url, Clock::time_point deadline, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const std::string port = std::to_string(url.port);
  if (const int rc = ::getaddrinfo(url.host.c_str(), port.c_str(), &hints, &found); rc != 0) {
    error = std::string("resolve ") + url.host + ": " + gai_strerror(rc);
    return -1;
  }
  int result = -1;
  for (addrinfo* item = found; item && result < 0; item = item->ai_next) {
    int fd = ::socket(item->ai_family, item->ai_socktype | SOCK_CLOEXEC, item->ai_protocol);
    if (fd < 0) continue;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, item->ai_addr, item->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      if (wait_fd(fd, POLLOUT, deadline)) {
        int so_error = 0;
        socklen_t length = sizeof so_error;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length);
        rc = so_error == 0 ? 0 : -1;
        if (so_error) errno = so_error;
      } else {
        errno = ETIMEDOUT;
      }
    }
    if (rc == 0) {
      result = fd;
    } else {
      error = std::string("connect ") + url.host + ":" + port + ": " + std::strerror(errno);
      ::close(fd);
    }
  }
  ::freeaddrinfo(found);
  return result;
}

bool send_all(int fd, const std::string& data, Clock::time_point deadline, std::string& error) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      if (!wait_fd(fd, POLLOUT, deadline)) {
        error = "send timeout";
        return false;
      }
    } else {
      error = std::string("send: ") + std::strerror(errno);
      return false;
    }
  }
  return true;
}

bool decode_chunked(const std::string& raw, std::string& out) {
  std::size_t pos = 0;
  for (;;) {
    const auto line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos) return false;
    const std::size_t size = std::stoul(raw.substr(pos, line_end - pos), nullptr, 16);
    pos = line_end + 2;
    if (size == 0) return true;
    if (pos + size > raw.size()) return false;
    out.append(raw, pos, size);
    pos += size + 2;
  }
}

}  // namespace

Url parse_url(const std::string& url) {
  const std::string scheme = "http://";
  if (url.rfind(scheme, 0) != 0) throw std::invalid_argument("only http:// URLs are supported: " + url);
  const std::string rest = url.substr(scheme.size());
  const auto slash = rest.find('/');
  const std::string authority = rest.substr(0, slash);
  Url result;
  result.path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (!authority.empty() && authority.front() == '[') {  // [::1]:8080
    const auto close = authority.find(']');
    if (close == std::string::npos) throw std::invalid_argument("bad host in " + url);
    result.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') result.port = std::stoi(authority.substr(close + 2));
  } else {
    const auto colon = authority.rfind(':');
    result.host = authority.substr(0, colon);
    if (colon != std::string::npos) result.port = std::stoi(authority.substr(colon + 1));
  }
  if (result.host.empty()) throw std::invalid_argument("missing host in " + url);
  return result;
}

Response request(const std::string& method, const std::string& url_text, const std::string& body,
                 const std::string& content_type, int timeout_ms) {
  Response response;
  Url url;
  try {
    url = parse_url(url_text);
  } catch (const std::exception& error) {
    response.error = error.what();
    return response;
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  Socket socket{connect_to(url, deadline, response.error)};
  if (socket.fd < 0) return response;

  std::string head = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                     "\r\nConnection: close\r\nAccept: */*\r\nUser-Agent: homemind-native\r\n";
  if (!content_type.empty()) head += "Content-Type: " + content_type + "\r\n";
  if (method != "GET" || !body.empty()) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  head += "\r\n";
  if (!send_all(socket.fd, head + body, deadline, response.error)) return response;

  std::string raw;
  char buffer[16384];
  for (;;) {
    const ssize_t n = ::recv(socket.fd, buffer, sizeof buffer, 0);
    if (n > 0) {
      raw.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) break;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      if (!wait_fd(socket.fd, POLLIN, deadline)) {
        response.error = "timeout after " + std::to_string(timeout_ms) + " ms";
        return response;
      }
      continue;
    }
    response.error = std::string("recv: ") + std::strerror(errno);
    return response;
  }
  const auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos || raw.rfind("HTTP/1.", 0) != 0) {
    response.error = "malformed HTTP response";
    return response;
  }
  const auto status_end = raw.find("\r\n");
  const std::string status_line = raw.substr(0, status_end);
  const auto space = status_line.find(' ');
  try {
    response.status = std::stoi(status_line.substr(space + 1, 3));
  } catch (const std::exception&) {
    response.error = "bad status line";
    return response;
  }
  std::size_t pos = status_end + 2;
  while (pos < header_end) {
    const auto line_end = raw.find("\r\n", pos);
    const std::string line = raw.substr(pos, line_end - pos);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      std::string value = line.substr(colon + 1);
      value.erase(0, value.find_first_not_of(' '));
      response.headers[lower(line.substr(0, colon))] = value;
    }
    pos = line_end + 2;
  }
  std::string payload = raw.substr(header_end + 4);
  if (lower(response.headers["transfer-encoding"]).find("chunked") != std::string::npos) {
    std::string decoded;
    if (!decode_chunked(payload, decoded)) {
      response.error = "truncated chunked body";
      response.status = 0;
      return response;
    }
    payload = std::move(decoded);
  }
  response.body = std::move(payload);
  return response;
}

Response post_multipart(const std::string& url, const std::vector<FormPart>& parts, int timeout_ms) {
  const std::string boundary = "----homemind" + new_id();
  std::string body;
  for (const auto& part : parts) {
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + part.name + "\"";
    if (!part.filename.empty()) {
      body += "; filename=\"" + part.filename + "\"\r\nContent-Type: " +
              (part.content_type.empty() ? std::string("application/octet-stream") : part.content_type);
    }
    body += "\r\n\r\n" + part.value + "\r\n";
  }
  body += "--" + boundary + "--\r\n";
  return request("POST", url, body, "multipart/form-data; boundary=" + boundary, timeout_ms);
}

}  // namespace hm::http
