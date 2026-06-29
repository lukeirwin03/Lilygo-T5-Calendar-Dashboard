#pragma once
#include <cstdint>
#include "secrets.h"

namespace config {
  // -- WiFi -- (credentials in secrets.h)
  constexpr const char* WIFI_SSID     = secrets::WIFI_SSID;
  constexpr const char* WIFI_PASSWORD = secrets::WIFI_PASSWORD;

  // -- MQTT --
  constexpr const char*    MQTT_HOST      = "192.168.1.38";
  constexpr uint16_t       MQTT_PORT      = 1883;
  constexpr const char*    MQTT_USER      = secrets::MQTT_USER;
  constexpr const char*    MQTT_PASS      = secrets::MQTT_PASS;
  constexpr const char*    MQTT_CLIENT_ID = "t5-calendar-dashboard";
  constexpr const char*    MQTT_TOPIC     = "dashboard/calendar";

  // -- Version tag shown in the header --
  constexpr const char*    VERSION_TAG    = "v2.7";

  // -- Power / sleep --
  // Scheduled data refresh interval (ms). Default 2 hours.
  constexpr unsigned long SCHEDULED_INTERVAL_MS = 2UL * 60UL * 60UL * 1000UL;
  // Sleep after this many ms of no touch activity while awake.
  constexpr unsigned long INACTIVITY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  // Hard cap on how long the scheduled wake waits for MQTT payload.
  constexpr unsigned long PAYLOAD_WAIT_MS = 8000;
  // SNTP sync timeout on cold boot.
  constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 10000;

  // -- Hardware pins (LilyGo T5 4.7" S3) --
  // These match the official LilyGo-EPD47 utilities.h for ESP32-S3.
  constexpr int BUTTON_PIN       = 21;   // physical button (RTC-capable)
  constexpr int BATTERY_ADC_PIN  = 14;   // battery voltage divider
  constexpr int TOUCH_SDA        = 18;
  constexpr int TOUCH_SCL        = 17;
  // NOTE: LilyGo-EPD47's utilities.h already defines TOUCH_INT as a macro.
  // Use TOUCH_INT_PIN here to avoid conflicts.
  constexpr int TOUCH_INT_PIN    = 47;   // GT911 IRQ (non-RTC; see note in power_mgr.cpp)
  // If you bridge GPIO10 to TOUCH_INT for deep-sleep wake, uncomment usage in power_mgr.cpp:
  constexpr int TOUCH_WAKE_PIN   = 10;   // RTC-capable GPIO for ext1 touch wake

  constexpr int BATTERY_SAMPLES   = 16;
  constexpr float BATTERY_DIVIDER = 2.0f;

  // -- Time --
  constexpr const char* TIMEZONE      = "CST6CDT,M3.2.0/2,M11.1.0/2";
  constexpr const char* NTP_SERVER_1  = "pool.ntp.org";
  constexpr const char* NTP_SERVER_2  = "time.nist.gov";

  // -- Diagnostics --
  constexpr unsigned long HEARTBEAT_MS = 30000;
}
