#include "terminal_qr.hpp"

#include <qrcodegen.hpp>

namespace hm::mobile {

namespace {
constexpr int kQuiet = 4;
const char* kUpper = "\xE2\x96\x80";  // U+2580 upper half block: foreground = top module, background = bottom
}  // namespace

int terminal_qr_size(const std::string& payload) {
  return qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM).getSize();
}

std::string render_terminal_qr(const std::string& payload) {
  const auto qr = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
  const int size = qr.getSize();
  auto dark = [&](int x, int y) {
    return x >= 0 && y >= 0 && x < size && y < size && qr.getModule(x, y);
  };
  std::string out;
  for (int y = -kQuiet; y < size + kQuiet; y += 2) {
    for (int x = -kQuiet; x < size + kQuiet; ++x) {
      out += dark(x, y) ? "\x1b[30m" : "\x1b[97m";          // top module
      out += dark(x, y + 1) ? "\x1b[40m" : "\x1b[107m";     // bottom module
      out += kUpper;
    }
    out += "\x1b[0m\n";
  }
  return out;
}

}  // namespace hm::mobile
