// Wi-Fi and MQTT broker address of the HomeMind model, the same for every board. The user decided on
// 2026-10-25 that this Wi-Fi may be in the code (it is not secret), so whoever flashes a board gets it with
// the source. The MQTT passwords are secret and stay in include/secrets.h (gitignored).
#pragma once

#define HM_SITE_WIFI_SSID "YOUR_WIFI_SSID"
#define HM_SITE_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// MQTT broker on NODE-RPI4-01 (authenticated listener, device-registry development_ip).
#define HM_SITE_MQTT_HOST "10.10.10.2"
#define HM_SITE_MQTT_PORT 1884
