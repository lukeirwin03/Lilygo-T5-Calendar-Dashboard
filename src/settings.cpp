#include "settings.h"
#include "config.h"
#include "sd_storage.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

namespace settings {

static Data s_data;
static bool s_initialized = false;

void resetToDefaults() {
  s_data.day_start_hour = 7;
  s_data.day_end_hour = 22;
  s_data.time_format_24h = false;
  s_data.context_days = 7;
  s_data.refresh_interval_s = 7200;
  s_data.inactivity_timeout_s = 180;
  s_data.sleep_start_hour = 22;
  s_data.sleep_end_hour = 7;
  s_data.history_retention_d = 365;
}

void init() {
  resetToDefaults();

  // Try to load from SD card
  char buf[2048];
  if (sd_storage::isMounted() && sd_storage::loadConfig(buf, sizeof(buf))) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (!err) {
      JsonObject obj = doc.as<JsonObject>();
      s_data.day_start_hour = obj["day_start_hour"] | s_data.day_start_hour;
      s_data.day_end_hour = obj["day_end_hour"] | s_data.day_end_hour;
      s_data.time_format_24h = obj["time_format_24h"] | s_data.time_format_24h;
      s_data.refresh_interval_s = obj["refresh_interval_s"] | s_data.refresh_interval_s;
      s_data.inactivity_timeout_s = obj["inactivity_timeout_s"] | s_data.inactivity_timeout_s;
      s_data.sleep_start_hour = obj["sleep_start_hour"] | s_data.sleep_start_hour;
      s_data.sleep_end_hour = obj["sleep_end_hour"] | s_data.sleep_end_hour;
      s_data.history_retention_d = obj["history_retention_d"] | s_data.history_retention_d;
      s_data.context_days = obj["context_days"] | s_data.context_days;
      // Schema robustness: clamp any out-of-range/legacy stored value back to
      // the default so a bad or old settings.json can never break the window.
      if (s_data.context_days < 1 || s_data.context_days > config::MAX_CONTEXT_DAYS) {
        s_data.context_days = 7;
      }

      Serial.println("[settings] Loaded from SD card");
    } else {
      Serial.println("[settings] Parse error, using defaults");
    }
  } else {
    Serial.println("[settings] No config file, using defaults");
  }

  s_initialized = true;
}

const Data& get() {
  if (!s_initialized) resetToDefaults();
  return s_data;
}

Data& mutableRef() {
  if (!s_initialized) resetToDefaults();
  return s_data;
}

bool save() {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  obj["day_start_hour"] = s_data.day_start_hour;
  obj["day_end_hour"] = s_data.day_end_hour;
  obj["time_format_24h"] = s_data.time_format_24h;
  obj["refresh_interval_s"] = (uint32_t)s_data.refresh_interval_s;
  obj["inactivity_timeout_s"] = (uint32_t)s_data.inactivity_timeout_s;
  obj["sleep_start_hour"] = s_data.sleep_start_hour;
  obj["sleep_end_hour"] = s_data.sleep_end_hour;
  obj["history_retention_d"] = (uint32_t)s_data.history_retention_d;
  obj["context_days"] = s_data.context_days;

  String output;
  serializeJson(doc, output);
  return sd_storage::saveConfig(output.c_str(), output.length());
}

} // namespace settings
