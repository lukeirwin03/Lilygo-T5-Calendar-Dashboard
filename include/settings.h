#pragma once
#include <cstdint>

// Runtime settings that override config.h compile-time defaults.
// Loaded from /config/settings.json on the SD card at boot.
// If the file is missing or corrupt, falls back to defaults.

namespace settings {

struct Data {
  // Display
  uint8_t  day_start_hour;       // default 7
  uint8_t  day_end_hour;         // default 22

  // Power
  uint32_t refresh_interval_s;   // default 7200 (2 hours)
  uint32_t inactivity_timeout_s; // default 45
  uint32_t history_retention_d;  // default 365

  // Network
  char     wifi_ssid[33];
  char     wifi_password[65];
  char     mqtt_host[64];
  uint16_t mqtt_port;
  char     mqtt_topic[64];
};

// Global instance — call init() once at boot, then use get() everywhere.
void init();
const Data& get();
Data& mutableRef();  // for editing in the settings UI

// Save current settings to SD card. Returns true on success.
bool save();

// Reset to compile-time defaults (from config.h).
void resetToDefaults();

} // namespace settings
