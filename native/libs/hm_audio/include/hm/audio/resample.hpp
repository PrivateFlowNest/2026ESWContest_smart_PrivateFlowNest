// Polyphase resampling compatible with scipy.signal.resample_poly(x, up, down) for float32 input
// (default window ('kaiser', 5.0), padtype 'constant'): same FIR design (firwin, 20*max(up, down)+1 taps),
// coefficients rounded to float32 like scipy does for float32 x, and float32 accumulation in the same
// order as scipy's upfirdn. Differences come only from libm sin/i0 rounding in the filter design.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace hm::audio {

struct PolyphaseFilter {
  int up = 1;
  int down = 1;
  int half_len = 0;
  std::vector<float> taps;  // scipy h after `h *= up` and the n_pre_pad zeros (post padding is all zeros)
  std::int64_t pre_remove = 0;  // scipy n_pre_remove
};

// Reduced up/down (gcd) and the scipy filter for them.
PolyphaseFilter design_resampler(int up, int down);
// up/down for converting `rate` Hz to `target` Hz (gcd-reduced).
void rational_factors(int rate, int target, int& up, int& down);

// scipy.signal.resample_poly(x, up, down) for float32 x (returns a copy when up == down).
std::vector<float> resample_poly(const std::vector<float>& x, int up, int down);

// Streaming form of the same filter for live capture: output sample k equals resample_poly on the whole
// stream (scipy semantics, zero history before the first sample) once its inputs have arrived.
class StreamResampler {
 public:
  StreamResampler(int up, int down);
  // Appends the outputs that became computable to `out`.
  void push(const float* input, std::size_t count, std::vector<float>& out);
  bool passthrough() const { return filter_.up == 1 && filter_.down == 1; }

 private:
  PolyphaseFilter filter_;
  std::deque<float> history_;     // input samples [history_start_, history_start_ + size)
  std::int64_t history_start_ = 0;
  std::int64_t received_ = 0;     // input samples seen
  std::int64_t next_output_ = 0;  // next output index (after pre_remove)
};

}  // namespace hm::audio
