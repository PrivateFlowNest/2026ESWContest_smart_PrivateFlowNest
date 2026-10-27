// QR pairing and bearer tokens for the Mobile API (same files and algorithm as the Python reference
// backend/app/auth.py, so a code printed by either side is accepted by the other).
//
//   pairing secret   <pairing_secret_file>: first line = 64 hex chars (32 random bytes), mode 0600. Created by the
//                    service (or the CLI) when missing. Never leaves the device.
//   nonce            "v1.<expires_at_unix_s>.<32 hex random>"
//   pairing code     base32(HMAC-SHA256(secret, "homemind-pairing-v1\n" + nonce)[:5]) -> 8 chars, shown "ABCD-EFGH".
//                    One-time (used nonces kept in <data_dir>/pairing-used.json until they expire), short-lived
//                    (default 600 s, at most 3600 s).
//   token            "hm1_" + 64 hex (32 random bytes). Only SHA-256(token) is stored, in <data_dir>/tokens.json
//                    (0600) with an 8-hex id, the phone's name and the creation time. Valid until revoked.
//   failed claims    at most N per client address and M overall in a sliding window -> 429 + Retry-After.
// Codes and tokens are never logged.
#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hm/common.hpp"

namespace hm::mobile {

inline constexpr int kMaxCodeTtlS = 3600;
inline constexpr int kMinCodeTtlS = 30;

struct AuthConfig {
  std::filesystem::path data_dir;
  std::filesystem::path secret_file;  // default <data_dir>/pairing-secret
  std::string device_id = "NODE-RPI4-01";
  std::string device_name = "HomeMind NODE-RPI4-01";
  std::string public_base_url;  // what the phone uses to reach this API (QR base_url)
  std::string tls_sha256;       // HTTPS certificate fingerprint for the QR (set at start when tls_enabled)
  int code_ttl_s = 600;
  int max_failures_per_client = 5;
  int max_failures_global = 20;
  int failure_window_s = 300;
};

// Files ----------------------------------------------------------------------------------------------------------
// mkdir -p; a directory this call creates gets mode 0700.
void ensure_private_dir(const std::filesystem::path& dir);
// Write via a 0600 temp file + rename. When running as root the file gets the owner of its directory, so the
// service user can still read what a person changed with the CLI.
void write_private_file(const std::filesystem::path& path, const std::string& content);

// Pairing secret / code ------------------------------------------------------------------------------------------
// Creates the secret when missing. Returns true when it was created now.
bool ensure_pairing_secret(const std::filesystem::path& path);
std::string load_pairing_secret(const std::filesystem::path& path);  // 32 raw bytes
std::string pairing_code(std::string_view secret, std::string_view nonce);  // 8 base32 chars, no dash
std::string format_code(const std::string& code);                          // "ABCD-EFGH"
std::string normalize_code(std::string_view code);                         // upper case, no '-' or spaces
// "v1.<expires>.<hex>" -> expires (unix seconds); nullopt when malformed.
std::optional<std::int64_t> nonce_expiry(std::string_view nonce);
std::string make_nonce(std::int64_t expires_at_s);
// Control characters removed, spaces trimmed, "unnamed" when empty.
std::string clean_device_name(std::string_view name);

struct PairingOffer {
  std::string code;  // formatted
  std::string nonce;
  std::int64_t expires_at_s = 0;
  json qr;  // payload the app parses (lib/models/pairing_models.dart, version 1)
};

// Tokens ---------------------------------------------------------------------------------------------------------
struct TokenRecord {
  std::string id;
  std::string sha256;
  std::string device_name;
  std::string created_at;
};

class TokenStore {
 public:
  explicit TokenStore(std::filesystem::path data_dir);

  // Token id when the token is valid. Re-reads tokens.json when it changed on disk (CLI revocation).
  std::optional<std::string> verify(std::string_view token);
  // Issues a new token; `token_out` receives the only plaintext copy.
  TokenRecord issue(const std::string& device_name, std::string& token_out);
  std::vector<TokenRecord> list();
  // Time (ms) of each token's last accepted verify() since this process started: the phone's last request.
  std::map<std::string, std::int64_t> last_seen();
  bool revoke(const std::string& id);
  std::size_t revoke_all();
  const std::filesystem::path& path() const { return path_; }

 private:
  struct Signature {
    std::int64_t mtime_ns = -1;
    std::int64_t size = -1;
    std::uint64_t inode = 0;
    bool operator==(const Signature&) const = default;
  };
  Signature signature() const;
  void load_locked(bool force);
  void save_locked();

  std::filesystem::path data_dir_;
  std::filesystem::path path_;
  std::mutex mutex_;
  std::vector<TokenRecord> records_;
  std::map<std::string, std::int64_t> last_seen_ms_;
  Signature loaded_;
  bool corrupt_ = false;
};

// "Bearer <token>" -> token (scheme case-insensitive). nullopt when absent or malformed.
std::optional<std::string> bearer_token(std::string_view authorization);

// Failed-claim limiter -------------------------------------------------------------------------------------------
class FailureLimiter {
 public:
  FailureLimiter(int per_client, int global, int window_s);
  // Seconds until the client may try again (0 = allowed).
  int blocked_for(const std::string& client, std::int64_t now_ms);
  void record(const std::string& client, std::int64_t now_ms);

 private:
  void prune(std::int64_t now_ms);

  int per_client_;
  int global_;
  std::int64_t window_ms_;
  std::mutex mutex_;
  std::deque<std::pair<std::int64_t, std::string>> failures_;  // (time, client), oldest first
};

// Pairing --------------------------------------------------------------------------------------------------------
class PairingService {
 public:
  PairingService(AuthConfig config, TokenStore& tokens);

  enum class Status { ok, invalid, rate_limited };
  struct ClaimResult {
    Status status = Status::invalid;
    std::string token;  // plaintext, only on ok
    TokenRecord record;
    int retry_after_s = 0;
  };

  // Ensures the data dir and the secret exist (service start).
  void prepare();
  PairingOffer make_offer(std::int64_t now_s, const std::string& base_url, int ttl_s) const;
  ClaimResult claim(const std::string& code, const std::string& nonce, const std::string& device_name,
                    const std::string& client, std::int64_t now_ms);
  const AuthConfig& config() const { return config_; }

 private:
  // Empty string when the code is good, else the reason (logged, never sent to the client).
  std::string check_locked(const std::string& code, const std::string& nonce, std::int64_t now_s);
  void load_used_locked(std::int64_t now_s);
  void save_used_locked();

  AuthConfig config_;
  TokenStore& tokens_;
  FailureLimiter limiter_;
  std::mutex mutex_;
  std::map<std::string, std::int64_t> used_;  // nonce -> expires_at_s
  bool used_loaded_ = false;
};

}  // namespace hm::mobile
