// homemind-perception: NODE-RPI5-02 microphone -> context path + voice path -> pfn.v1 MQTT.
//   homemind-perception --config /etc/homemind/perception.json
//   homemind-perception --config cfg.json --wav clip.wav --fast --stdout     (file mode, PC checks)
//   homemind-perception --list-devices
#include "runner.hpp"

int main(int argc, char** argv) { return hm::perception::run_cli(argc, argv); }
