// Small crypto helpers for pairing and tokens: SHA-256 (FIPS 180-4), HMAC-SHA-256 (RFC 2104), getrandom(2),
// hex and RFC 4648 base32, constant-time comparison. No OpenSSL dependency (the dev image has no libssl-dev,
// and the service only needs hashing and a MAC). Checked against the FIPS/RFC 4231 vectors in the unit tests
// and against Python hashlib/hmac in native/tests/test_mobile_api_parity.py.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hm::crypto {

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(std::string_view data);
Digest hmac_sha256(std::string_view key, std::string_view message);

// Lowercase hex of bytes.
std::string hex(const std::uint8_t* data, std::size_t size);
inline std::string hex(const Digest& digest) { return hex(digest.data(), digest.size()); }
std::string sha256_hex(std::string_view data);
// Hex text -> bytes. Throws std::invalid_argument on odd length or a non-hex character.
std::string unhex(std::string_view text);

// RFC 4648 base32 (A-Z, 2-7), no padding.
std::string base32(const std::uint8_t* data, std::size_t size);

// Cryptographically secure random bytes from the kernel (getrandom). Throws std::runtime_error on failure.
std::string random_bytes(std::size_t count);
std::string random_hex(std::size_t byte_count);

// Equal length and content, without an early exit on the first difference.
bool constant_time_equal(std::string_view a, std::string_view b);

}  // namespace hm::crypto
