#include "hm/audio/resample.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "hm/audio/common.hpp"

namespace hm::audio {

namespace {

constexpr double kPi = 3.141592653589793;  // numpy.pi

// Modified Bessel function I0 by its power series (x <= 5 here; ~1 ulp from scipy.special.i0).
double bessel_i0(double x) {
  const double quarter = x * x / 4.0;
  double term = 1.0;
  double sum = 1.0;
  for (int k = 1; k < 200; ++k) {
    term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-18) break;
  }
  return sum;
}

std::int64_t ceil_div(std::int64_t a, std::int64_t b) { return a <= 0 ? 0 : (a + b - 1) / b; }

}  // namespace

void rational_factors(int rate, int target, int& up, int& down) {
  if (rate <= 0 || target <= 0) throw std::invalid_argument("sample rates must be positive");
  const int g = std::gcd(rate, target);
  up = target / g;
  down = rate / g;
}

PolyphaseFilter design_resampler(int up, int down) {
  if (up < 1 || down < 1) throw std::invalid_argument("up and down must be >= 1");
  const int g = std::gcd(up, down);
  PolyphaseFilter filter;
  filter.up = up / g;
  filter.down = down / g;
  if (filter.up == 1 && filter.down == 1) return filter;
  // firwin(2 * half_len + 1, 1 / max_rate, window=('kaiser', 5.0)), scale=True (DC gain 1).
  const int max_rate = std::max(filter.up, filter.down);
  const double cutoff = 1.0 / max_rate;
  filter.half_len = 10 * max_rate;
  const int numtaps = 2 * filter.half_len + 1;
  const double alpha = 0.5 * (numtaps - 1);
  const double window_alpha = (numtaps - 1) / 2.0;
  const double i0_beta = bessel_i0(5.0);
  std::vector<double> h(static_cast<std::size_t>(numtaps));
  for (int i = 0; i < numtaps; ++i) {
    const double m = static_cast<double>(i) - alpha;
    const double x = cutoff * m;
    const double y = kPi * (x != 0.0 ? x : DBL_EPSILON);  // array_api_extra sinc
    const double sinc = std::sin(y) / y;
    const double r = (static_cast<double>(i) - window_alpha) / window_alpha;
    const double window = bessel_i0(5.0 * std::sqrt(1.0 - r * r)) / i0_beta;
    h[static_cast<std::size_t>(i)] = (cutoff * sinc) * window;
  }
  const double scale = pairwise_sum(h.data(), h.size());
  // n_pre_pad zeros put the output samples at the centre; scipy's post padding is zeros only.
  const int pre_pad = filter.down - filter.half_len % filter.down;
  filter.pre_remove = (filter.half_len + pre_pad) / filter.down;
  filter.taps.assign(static_cast<std::size_t>(pre_pad), 0.0f);
  const float gain = static_cast<float>(filter.up);
  for (double value : h) filter.taps.push_back(static_cast<float>(value / scale) * gain);
  return filter;
}

std::vector<float> resample_poly(const std::vector<float>& x, int up, int down) {
  const PolyphaseFilter filter = design_resampler(up, down);
  if (filter.up == 1 && filter.down == 1) return x;
  const auto n_in = static_cast<std::int64_t>(x.size());
  const std::int64_t product = n_in * filter.up;
  const std::int64_t n_out = product / filter.down + (product % filter.down ? 1 : 0);
  const auto taps = static_cast<std::int64_t>(filter.taps.size());
  std::vector<float> y(static_cast<std::size_t>(n_out));
  for (std::int64_t j = 0; j < n_out; ++j) {
    const std::int64_t t = (filter.pre_remove + j) * filter.down;  // position in the upsampled stream
    const std::int64_t i_max = std::min(n_in - 1, t / filter.up);
    const std::int64_t i_min = ceil_div(t - (taps - 1), filter.up);
    float acc = 0.0f;
    for (std::int64_t i = i_min; i <= i_max; ++i) {
      acc = acc + x[static_cast<std::size_t>(i)] * filter.taps[static_cast<std::size_t>(t - i * filter.up)];
    }
    y[static_cast<std::size_t>(j)] = acc;
  }
  return y;
}

StreamResampler::StreamResampler(int up, int down) : filter_(design_resampler(up, down)) {}

void StreamResampler::push(const float* input, std::size_t count, std::vector<float>& out) {
  if (passthrough()) {
    out.insert(out.end(), input, input + count);
    received_ += static_cast<std::int64_t>(count);
    history_start_ = received_;
    return;
  }
  history_.insert(history_.end(), input, input + count);
  received_ += static_cast<std::int64_t>(count);
  const auto taps = static_cast<std::int64_t>(filter_.taps.size());
  for (;;) {
    const std::int64_t t = (filter_.pre_remove + next_output_) * filter_.down;
    const std::int64_t i_max = t / filter_.up;
    if (i_max >= received_) break;
    const std::int64_t i_min = ceil_div(t - (taps - 1), filter_.up);
    float acc = 0.0f;
    for (std::int64_t i = i_min; i <= i_max; ++i) {
      acc = acc + history_[static_cast<std::size_t>(i - history_start_)] *
                      filter_.taps[static_cast<std::size_t>(t - i * filter_.up)];
    }
    out.push_back(acc);
    ++next_output_;
  }
  const std::int64_t next_t = (filter_.pre_remove + next_output_) * filter_.down;
  const std::int64_t keep_from = ceil_div(next_t - (taps - 1), filter_.up);
  while (history_start_ < keep_from && !history_.empty()) {
    history_.pop_front();
    ++history_start_;
  }
}

}  // namespace hm::audio
