#include "hm/audio/common.hpp"

namespace hm::audio {

// numpy/_core/src/umath/loops_utils.h.src (PW_BLOCKSIZE 128, 8 accumulators).
double pairwise_sum(const double* a, std::size_t n) {
  if (n < 8) {
    double res = 0.0;
    for (std::size_t i = 0; i < n; ++i) res += a[i];
    return res;
  }
  if (n <= 128) {
    double r[8];
    for (int j = 0; j < 8; ++j) r[j] = a[j];
    std::size_t i = 8;
    for (; i < n - (n % 8); i += 8) {
      for (int j = 0; j < 8; ++j) r[j] += a[i + static_cast<std::size_t>(j)];
    }
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += a[i];
    return res;
  }
  std::size_t n2 = n / 2;
  n2 -= n2 % 8;
  return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}

std::size_t utf8_length(const std::string& text) {
  std::size_t count = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::string utf8_prefix(const std::string& text, std::size_t limit) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      if (count == limit) return text.substr(0, i);
      ++count;
    }
  }
  return text;
}

}  // namespace hm::audio
