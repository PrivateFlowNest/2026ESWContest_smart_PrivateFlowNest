// Pairing QR drawn in the device terminal (ANSI colours + half blocks), so a person can show it to the phone
// without qrencode or any online generator. Uses the vendored Nayuki qrcodegen (native/third_party/qrcodegen).
#pragma once

#include <string>

namespace hm::mobile {

// Black modules on a white background with a 4-module quiet zone; two QR rows per text line.
std::string render_terminal_qr(const std::string& payload);
// Side length in modules (without quiet zone) of the QR for payload, for tests.
int terminal_qr_size(const std::string& payload);

}  // namespace hm::mobile
