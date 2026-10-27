#include "crypto.hpp"

#include <sys/random.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace hm::crypto {

namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }

void compress(std::uint32_t state[8], const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t(block[4 * i]) << 24) | (std::uint32_t(block[4 * i + 1]) << 16) |
           (std::uint32_t(block[4 * i + 2]) << 8) | std::uint32_t(block[4 * i + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6],
                h = state[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + s1 + choose + kRound[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

Digest sha256(std::string_view data) {
  std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t offset = 0;
  for (; offset + 64 <= data.size(); offset += 64) compress(state, bytes + offset);
  std::uint8_t tail[128] = {};
  const std::size_t rest = data.size() - offset;
  if (rest) std::memcpy(tail, bytes + offset, rest);
  tail[rest] = 0x80;
  const std::size_t tail_size = rest < 56 ? 64 : 128;
  const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = static_cast<std::uint8_t>(bit_length >> (8 * i));
  compress(state, tail);
  if (tail_size == 128) compress(state, tail + 64);
  Digest digest{};
  for (int i = 0; i < 8; ++i) {
    digest[4 * i] = static_cast<std::uint8_t>(state[i] >> 24);
    digest[4 * i + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    digest[4 * i + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    digest[4 * i + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return digest;
}

Digest hmac_sha256(std::string_view key, std::string_view message) {
  std::string block_key(64, '\0');
  if (key.size() > 64) {
    const Digest hashed = sha256(key);
    std::memcpy(block_key.data(), hashed.data(), hashed.size());
  } else {
    std::memcpy(block_key.data(), key.data(), key.size());
  }
  std::string inner(64, '\0'), outer(64, '\0');
  for (int i = 0; i < 64; ++i) {
    inner[i] = static_cast<char>(block_key[i] ^ 0x36);
    outer[i] = static_cast<char>(block_key[i] ^ 0x5c);
  }
  inner.append(message);
  const Digest inner_hash = sha256(inner);
  outer.append(reinterpret_cast<const char*>(inner_hash.data()), inner_hash.size());
  return sha256(outer);
}

std::string hex(const std::uint8_t* data, std::size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0F]);
  }
  return out;
}

std::string sha256_hex(std::string_view data) { return hex(sha256(data)); }

std::string unhex(std::string_view text) {
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (text.size() % 2) throw std::invalid_argument("odd hex length");
  std::string out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int high = value(text[i]);
    const int low = value(text[i + 1]);
    if (high < 0 || low < 0) throw std::invalid_argument("not hex");
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

std::string base32(const std::uint8_t* data, std::size_t size) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (std::size_t i = 0; i < size; ++i) {
    buffer = (buffer << 8) | data[i];
    bits += 8;
    while (bits >= 5) {
      out.push_back(alphabet[(buffer >> (bits - 5)) & 0x1F]);
      bits -= 5;
    }
  }
  if (bits > 0) out.push_back(alphabet[(buffer << (5 - bits)) & 0x1F]);
  return out;
}

std::string random_bytes(std::size_t count) {
  std::string out(count, '\0');
  std::size_t filled = 0;
  while (filled < count) {
    const ssize_t got = getrandom(out.data() + filled, count - filled, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("getrandom failed: ") + std::strerror(errno));
    }
    filled += static_cast<std::size_t>(got);
  }
  return out;
}

std::string random_hex(std::size_t byte_count) {
  const std::string bytes = random_bytes(byte_count);
  return hex(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace hm::crypto
