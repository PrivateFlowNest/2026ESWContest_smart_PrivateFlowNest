#include "auth.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "crypto.hpp"
#include "store.hpp"

namespace hm::mobile {

namespace fs = std::filesystem;

namespace {

constexpr const char* kCodeContext = "homemind-pairing-v1\n";
constexpr std::size_t kCodeBytes = 5;  // 40 bits -> 8 base32 characters
constexpr int kExpirySlackS = 60;

std::string errno_text() { return std::strerror(errno); }

fs::path directory_of(const fs::path& path) {
  return path.parent_path().empty() ? fs::path(".") : path.parent_path();
}

// When root writes a file (the CLI run by a person), hand it to the owner of its directory (the service user).
void match_directory_owner(int fd, const fs::path& path) {
  if (geteuid() != 0) return;
  struct stat dir_stat {};
  if (::stat(directory_of(path).c_str(), &dir_stat) != 0) return;
  if (fchown(fd, dir_stat.st_uid, dir_stat.st_gid) != 0) {
    log(LogLevel::warning, "auth", "cannot hand " + path.string() + " to the directory owner: " + errno_text());
  }
}

void write_all(int fd, const std::string& content, const fs::path& path) {
  std::size_t written = 0;
  while (written < content.size()) {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("cannot write " + path.string() + ": " + errno_text());
    }
    written += static_cast<std::size_t>(n);
  }
}

// Exclusive flock on the data directory for read-modify-write of tokens.json (service vs CLI).
class DirectoryLock {
 public:
  explicit DirectoryLock(const fs::path& dir) : fd_(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
    if (fd_ < 0) throw std::runtime_error("cannot open " + dir.string() + ": " + errno_text());
    while (::flock(fd_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        ::close(fd_);
        throw std::runtime_error("cannot lock " + dir.string() + ": " + errno_text());
      }
    }
  }
  ~DirectoryLock() {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;

 private:
  int fd_;
};

std::string trim(std::string_view text, const char* characters = " \t\r\n") {
  const auto begin = text.find_first_not_of(characters);
  if (begin == std::string_view::npos) return {};
  const auto end = text.find_last_not_of(characters);
  return std::string(text.substr(begin, end - begin + 1));
}

bool is_lower_hex(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

// Files ----------------------------------------------------------------------------------------------------------

void ensure_private_dir(const fs::path& dir) {
  if (fs::exists(dir)) return;
  fs::create_directories(dir);
  fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
}

void write_private_file(const fs::path& path, const std::string& content) {
  const fs::path tmp = directory_of(path) / ("." + path.filename().string() + ".tmp-" + crypto::random_hex(4));
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot create " + tmp.string() + ": " + errno_text());
  try {
    write_all(fd, content, tmp);
    if (::fsync(fd) != 0) throw std::runtime_error("fsync " + tmp.string() + ": " + errno_text());
    match_directory_owner(fd, path);
  } catch (...) {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const std::string error = errno_text();
    ::unlink(tmp.c_str());
    throw std::runtime_error("cannot replace " + path.string() + ": " + error);
  }
}

// Pairing secret / code ------------------------------------------------------------------------------------------

bool ensure_pairing_secret(const fs::path& path) {
  if (fs::exists(path)) return false;
  ensure_private_dir(directory_of(path));
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (errno == EEXIST) return false;
    throw std::runtime_error("cannot create pairing secret " + path.string() + ": " + errno_text());
  }
  try {
    write_all(fd, crypto::random_hex(32) + "\n", path);
    if (::fsync(fd) != 0) throw std::runtime_error("fsync " + path.string() + ": " + errno_text());
    match_directory_owner(fd, path);
  } catch (...) {
    ::close(fd);
    ::unlink(path.c_str());
    throw;
  }
  ::close(fd);
  return true;
}

std::string load_pairing_secret(const fs::path& path) {
  const std::string text = read_text_file(path);
  const std::string line = trim(text.substr(0, text.find_first_of("\r\n")));
  if (line.size() != 64 || !is_lower_hex(line)) {
    throw std::runtime_error("pairing secret " + path.string() + " must hold 64 lowercase hex characters");
  }
  return crypto::unhex(line);
}

std::string pairing_code(std::string_view secret, std::string_view nonce) {
  const crypto::Digest mac = crypto::hmac_sha256(secret, std::string(kCodeContext) + std::string(nonce));
  return crypto::base32(mac.data(), kCodeBytes);
}

std::string format_code(const std::string& code) {
  return code.size() == 8 ? code.substr(0, 4) + "-" + code.substr(4) : code;
}

std::string normalize_code(std::string_view code) {
  std::string out;
  for (const char c : code) {
    if (c == '-' || c == ' ') continue;
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

std::optional<std::int64_t> nonce_expiry(std::string_view nonce) {
  if (nonce.substr(0, 3) != "v1.") return std::nullopt;
  const std::string_view rest = nonce.substr(3);
  const auto dot = rest.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot > 12) return std::nullopt;
  const std::string_view digits = rest.substr(0, dot);
  const std::string_view random = rest.substr(dot + 1);
  if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
  if (random.size() != 32 || !is_lower_hex(random)) return std::nullopt;
  return std::stoll(std::string(digits));
}

std::string make_nonce(std::int64_t expires_at_s) {
  return "v1." + std::to_string(expires_at_s) + "." + crypto::random_hex(16);
}

std::string clean_device_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) continue;
    out.push_back(c);
  }
  out = trim(out, " ");
  return out.empty() ? "unnamed" : out;
}

// Tokens ---------------------------------------------------------------------------------------------------------

TokenStore::TokenStore(fs::path data_dir) : data_dir_(std::move(data_dir)), path_(data_dir_ / "tokens.json") {}

TokenStore::Signature TokenStore::signature() const {
  struct stat st {};
  if (::stat(path_.c_str(), &st) != 0) return {};
  return {static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec,
          static_cast<std::int64_t>(st.st_size), static_cast<std::uint64_t>(st.st_ino)};
}

void TokenStore::load_locked(bool force) {
  const Signature current = signature();
  if (!force && current == loaded_) return;
  records_.clear();
  corrupt_ = false;
  loaded_ = current;
  if (current.size < 0) return;  // no file yet: no tokens
  try {
    const json document = read_json_file(path_);
    for (const auto& item : document.at("tokens")) {
      TokenRecord record{item.at("id").get<std::string>(), item.at("sha256").get<std::string>(),
                         item.value("device_name", std::string("unnamed")), item.value("created_at", std::string())};
      if (record.sha256.size() != 64 || !is_lower_hex(record.sha256) || record.id.empty()) {
        log(LogLevel::warning, "auth", "ignoring a malformed entry in " + path_.string());
        continue;
      }
      records_.push_back(std::move(record));
    }
  } catch (const std::exception& error) {
    records_.clear();
    corrupt_ = true;  // fail closed: no token is accepted until the file is fixed
    log(LogLevel::error, "auth", path_.string() + " is unreadable; every token is refused: " + error.what());
  }
}

void TokenStore::save_locked() {
  json tokens = json::array();
  for (const auto& record : records_) {
    tokens.push_back({{"id", record.id},
                      {"sha256", record.sha256},
                      {"device_name", record.device_name},
                      {"created_at", record.created_at}});
  }
  ensure_private_dir(data_dir_);
  write_private_file(path_, json({{"version", 1}, {"tokens", tokens}}).dump(2, ' ', false) + "\n");
  loaded_ = signature();
  corrupt_ = false;
}

std::optional<std::string> TokenStore::verify(std::string_view token) {
  std::lock_guard lock(mutex_);
  load_locked(false);
  if (corrupt_) return std::nullopt;
  const std::string hash = crypto::sha256_hex(token);
  std::optional<std::string> found;
  for (const auto& record : records_) {
    if (crypto::constant_time_equal(record.sha256, hash)) found = record.id;
  }
  if (found) last_seen_ms_[*found] = now_ms();
  return found;
}

TokenRecord TokenStore::issue(const std::string& device_name, std::string& token_out) {
  std::lock_guard lock(mutex_);
  ensure_private_dir(data_dir_);
  DirectoryLock directory(data_dir_);
  load_locked(true);
  if (corrupt_) throw std::runtime_error(path_.string() + " is unreadable; fix or remove it before pairing");
  const std::string token = "hm1_" + crypto::random_hex(32);
  std::string id;
  do {
    id = crypto::random_hex(4);
  } while (std::any_of(records_.begin(), records_.end(), [&](const TokenRecord& r) { return r.id == id; }));
  TokenRecord record{id, crypto::sha256_hex(token), device_name, utc_iso_seconds(now_ms() / 1000)};
  records_.push_back(record);
  save_locked();
  token_out = token;
  return record;
}

std::vector<TokenRecord> TokenStore::list() {
  std::lock_guard lock(mutex_);
  load_locked(true);
  if (corrupt_) throw std::runtime_error(path_.string() + " is unreadable");
  return records_;
}

std::map<std::string, std::int64_t> TokenStore::last_seen() {
  std::lock_guard lock(mutex_);
  return last_seen_ms_;
}

bool TokenStore::revoke(const std::string& id) {
  std::lock_guard lock(mutex_);
  if (!fs::exists(data_dir_)) return false;
  DirectoryLock directory(data_dir_);
  load_locked(true);
  if (corrupt_) throw std::runtime_error(path_.string() + " is unreadable; use --revoke-all-tokens to reset it");
  const auto before = records_.size();
  records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const TokenRecord& r) { return r.id == id; }),
                 records_.end());
  if (records_.size() == before) return false;
  save_locked();
  last_seen_ms_.erase(id);
  return true;
}

std::size_t TokenStore::revoke_all() {
  std::lock_guard lock(mutex_);
  if (!fs::exists(data_dir_)) return 0;
  DirectoryLock directory(data_dir_);
  load_locked(true);
  const std::size_t count = records_.size();
  records_.clear();
  save_locked();  // also resets an unreadable file
  return count;
}

std::optional<std::string> bearer_token(std::string_view authorization) {
  const std::string value = trim(authorization);
  const auto space = value.find(' ');
  if (space == std::string::npos) return std::nullopt;
  std::string scheme = value.substr(0, space);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
  if (scheme != "bearer") return std::nullopt;
  std::string token = trim(value.substr(space + 1));
  if (token.empty() || token.size() > 512) return std::nullopt;
  return token;
}

// Failed-claim limiter -------------------------------------------------------------------------------------------

FailureLimiter::FailureLimiter(int per_client, int global, int window_s)
    : per_client_(per_client), global_(global), window_ms_(static_cast<std::int64_t>(window_s) * 1000) {}

void FailureLimiter::prune(std::int64_t now_ms) {
  while (!failures_.empty() && failures_.front().first + window_ms_ <= now_ms) failures_.pop_front();
}

int FailureLimiter::blocked_for(const std::string& client, std::int64_t now_ms) {
  std::lock_guard lock(mutex_);
  prune(now_ms);
  std::int64_t wait_ms = 0;
  if (global_ > 0 && static_cast<int>(failures_.size()) >= global_) {
    wait_ms = std::max(wait_ms, failures_[failures_.size() - global_].first + window_ms_ - now_ms);
  }
  std::vector<std::int64_t> mine;
  for (const auto& [at, who] : failures_) {
    if (who == client) mine.push_back(at);
  }
  if (per_client_ > 0 && static_cast<int>(mine.size()) >= per_client_) {
    wait_ms = std::max(wait_ms, mine[mine.size() - per_client_] + window_ms_ - now_ms);
  }
  if (wait_ms <= 0) return 0;
  return static_cast<int>((wait_ms + 999) / 1000);
}

void FailureLimiter::record(const std::string& client, std::int64_t now_ms) {
  std::lock_guard lock(mutex_);
  prune(now_ms);
  failures_.emplace_back(now_ms, client);
}

// Pairing --------------------------------------------------------------------------------------------------------

PairingService::PairingService(AuthConfig config, TokenStore& tokens)
    : config_(std::move(config)),
      tokens_(tokens),
      limiter_(config_.max_failures_per_client, config_.max_failures_global, config_.failure_window_s) {
  if (config_.secret_file.empty()) config_.secret_file = config_.data_dir / "pairing-secret";
}

void PairingService::prepare() {
  ensure_private_dir(config_.data_dir);
  if (ensure_pairing_secret(config_.secret_file)) {
    log(LogLevel::info, "auth", "created pairing secret " + config_.secret_file.string());
  }
  struct stat st {};
  if (::stat(config_.secret_file.c_str(), &st) == 0 && (st.st_mode & 0007)) {
    log(LogLevel::warning, "auth", config_.secret_file.string() + " is readable by other users; chmod 0600 it");
  }
  load_pairing_secret(config_.secret_file);  // fail at start, not at the first claim
}

PairingOffer PairingService::make_offer(std::int64_t now_s, const std::string& base_url, int ttl_s) const {
  if (ttl_s < kMinCodeTtlS || ttl_s > kMaxCodeTtlS) {
    throw std::invalid_argument("pairing code lifetime must be " + std::to_string(kMinCodeTtlS) + ".." +
                                std::to_string(kMaxCodeTtlS) + " s");
  }
  if (base_url.rfind("http://", 0) != 0 && base_url.rfind("https://", 0) != 0) {
    throw std::invalid_argument("base URL must start with http:// or https:// (set public_base_url or --base-url)");
  }
  PairingOffer offer;
  offer.expires_at_s = now_s + ttl_s;
  offer.nonce = make_nonce(offer.expires_at_s);
  offer.code = format_code(pairing_code(load_pairing_secret(config_.secret_file), offer.nonce));
  std::string url = base_url;
  while (url.size() > 1 && url.back() == '/') url.pop_back();
  offer.qr = {{"version", 1},
              {"device_id", config_.device_id},
              {"device_name", config_.device_name},
              {"base_url", url},
              {"pairing_code", offer.code},
              {"nonce", offer.nonce},
              {"expires_at", utc_iso_seconds(offer.expires_at_s)}};
  // HTTPS: the app trusts exactly this certificate (pinning); plain http:// QRs never carry it
  if (!config_.tls_sha256.empty() && url.rfind("https://", 0) == 0) offer.qr["tls_sha256"] = config_.tls_sha256;
  return offer;
}

void PairingService::load_used_locked(std::int64_t now_s) {
  if (!used_loaded_) {
    used_loaded_ = true;
    const fs::path path = config_.data_dir / "pairing-used.json";
    if (fs::exists(path)) {
      try {
        const json document = read_json_file(path);
        for (const auto& [nonce, expires] : document.at("used").items()) used_[nonce] = expires.get<std::int64_t>();
      } catch (const std::exception& error) {
        log(LogLevel::error, "auth", path.string() + " is unreadable: " + error.what());
      }
    }
  }
  for (auto it = used_.begin(); it != used_.end();) {
    it = it->second < now_s ? used_.erase(it) : std::next(it);
  }
}

void PairingService::save_used_locked() {
  json used = json::object();
  for (const auto& [nonce, expires] : used_) used[nonce] = expires;
  ensure_private_dir(config_.data_dir);
  write_private_file(config_.data_dir / "pairing-used.json", json({{"version", 1}, {"used", used}}).dump(2) + "\n");
}

std::string PairingService::check_locked(const std::string& code, const std::string& nonce, std::int64_t now_s) {
  const auto expires = nonce_expiry(nonce);
  if (!expires) return "malformed nonce";
  if (now_s >= *expires) return "expired";
  if (*expires > now_s + kMaxCodeTtlS + kExpirySlackS) return "expiry beyond the maximum lifetime";
  const std::string expected = pairing_code(load_pairing_secret(config_.secret_file), nonce);
  if (!crypto::constant_time_equal(normalize_code(code), expected)) return "wrong code";
  if (used_.count(nonce)) return "already used";
  return {};
}

PairingService::ClaimResult PairingService::claim(const std::string& code, const std::string& nonce,
                                                  const std::string& device_name, const std::string& client,
                                                  std::int64_t now_ms) {
  ClaimResult result;
  if (const int wait = limiter_.blocked_for(client, now_ms); wait > 0) {
    result.status = Status::rate_limited;
    result.retry_after_s = wait;
    log(LogLevel::warning, "auth", "pairing claim from " + client + " refused: too many failed attempts");
    return result;
  }
  std::lock_guard lock(mutex_);
  const std::int64_t now_s = now_ms / 1000;
  load_used_locked(now_s);
  const std::string reason = check_locked(code, nonce, now_s);
  if (!reason.empty()) {
    limiter_.record(client, now_ms);
    result.status = Status::invalid;
    log(LogLevel::warning, "auth", "pairing claim from " + client + " refused (" + reason + ")");
    return result;
  }
  used_[nonce] = *nonce_expiry(nonce);
  save_used_locked();
  result.record = tokens_.issue(clean_device_name(device_name), result.token);
  result.status = Status::ok;
  log(LogLevel::info, "auth",
      "token " + result.record.id + " issued to '" + result.record.device_name + "' from " + client);
  return result;
}

}  // namespace hm::mobile
