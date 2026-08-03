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

  // -- Power / sleep --
  // Sleep after this many ms of no touch activity while awake.
  constexpr unsigned long INACTIVITY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  // Hard cap on how long the scheduled wake waits for MQTT payload.
  constexpr unsigned long PAYLOAD_WAIT_MS = 8000;
  // SNTP sync timeout on cold boot.
  constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 10000;
  // Wake this many seconds before an event starts, giving the display time to
  // refresh so upcoming events appear with a comfortable lead.
  constexpr unsigned long EVENT_WAKE_LEAD_S = 600;  // 10 minutes
  // Only load events within this many days from today into memory. Events
  // outside this window are still persisted to SD (history) but not held in
  // RAM. 12 days covers the visible week plus lookahead for scheduling.
  constexpr int MAX_EVENT_WINDOW_DAYS = 12;

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

  // -- SD card (SPI) --
  constexpr int SDCARD_MISO = 16;
  constexpr int SDCARD_MOSI = 15;
  constexpr int SDCARD_SCK  = 11;
  constexpr int SDCARD_CS   = 42;

  // -- Time --
  constexpr const char* TIMEZONE      = "CST6CDT,M3.2.0/2,M11.1.0/2";
  constexpr const char* NTP_SERVER_1  = "pool.ntp.org";
  constexpr const char* NTP_SERVER_2  = "time.nist.gov";

  // -- Diagnostics --
  constexpr unsigned long HEARTBEAT_MS = 30000;
}
