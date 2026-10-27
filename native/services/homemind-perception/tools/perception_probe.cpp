// PC test probe for homemind-perception (never shipped): one JSON request per stdin line, one JSON answer per line.
//   {"op": "voice_listen", "document": {...}}  -> {"problem": ""} ("" = schema-valid, as voice_listen.hpp decides)
#include <iostream>
#include <string>

#include "hm/common.hpp"
#include "voice_listen.hpp"

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
    hm::json out;
    try {
      const hm::json request = hm::json::parse(line);
      if (request.value("op", "") == "voice_listen") {
        out = {{"problem", hm::perception::validate_voice_listen(request.at("document"))}};
      } else {
        out = {{"error", "unknown op"}};
      }
    } catch (const std::exception& error) {
      out = {{"error", error.what()}};
    }
    std::cout << out.dump(-1, ' ', false, hm::json::error_handler_t::replace) << std::endl;
  }
  return 0;
}
