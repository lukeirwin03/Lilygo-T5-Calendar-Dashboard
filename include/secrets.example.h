#pragma once

// Copy to `include/secrets.h` and fill in your network credentials.
// `include/secrets.h` should NOT be committed (see .gitignore).

namespace secrets {
  constexpr const char* WIFI_SSID     = "your-ssid";
  constexpr const char* WIFI_PASSWORD = "your-password";

  constexpr const char* MQTT_USER = nullptr;   // or "user" if your broker requires auth
  constexpr const char* MQTT_PASS = nullptr;
}
