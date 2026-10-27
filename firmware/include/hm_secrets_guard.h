// Includes the site network (include/hm_site_network.h, committed) and include/secrets.h (MQTT passwords,
// gitignored), and stops the build with a clear message if secrets.h is missing or incomplete. Values are
// never printed by the firmware.
#pragma once

#include "hm_site_network.h"

#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "firmware/include/secrets.h is missing: copy include/secrets.h.example to include/secrets.h and fill in the MQTT password (it is gitignored, never commit it)"
#endif
#else
#include "secrets.h"
#endif

#if !defined(HM_MQTT_PASSWORD_ESP32_ACT) || !defined(HM_MQTT_PASSWORD_ESP8266_LED) || !defined(HM_NTP_SERVER)
#error "firmware/include/secrets.h is incomplete: compare it with include/secrets.h.example"
#endif
