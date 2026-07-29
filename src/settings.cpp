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
  s_data.refresh_interval_s = 7200;
  s_data.inactivity_timeout_s = 45;
  s_data.history_retention_d = 365;
  strlcpy(s_data.wifi_ssid, config::WIFI_SSID, sizeof(s_data.wifi_ssid));
  strlcpy(s_data.wifi_password, config::WIFI_PASSWORD, sizeof(s_data.wifi_password));
  strlcpy(s_data.mqtt_host, config::MQTT_HOST, sizeof(s_data.mqtt_host));
  s_data.mqtt_port = config::MQTT_PORT;
  strlcpy(s_data.mqtt_topic, config::MQTT_TOPIC, sizeof(s_data.mqtt_topic));
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
      s_data.refresh_interval_s = obj["refresh_interval_s"] | s_data.refresh_interval_s;
      s_data.inactivity_timeout_s = obj["inactivity_timeout_s"] | s_data.inactivity_timeout_s;
      s_data.history_retention_d = obj["history_retention_d"] | s_data.history_retention_d;

      if (obj.containsKey("wifi_ssid"))
        strlcpy(s_data.wifi_ssid, obj["wifi_ssid"], sizeof(s_data.wifi_ssid));
      if (obj.containsKey("wifi_password"))
        strlcpy(s_data.wifi_password, obj["wifi_password"], sizeof(s_data.wifi_password));
      if (obj.containsKey("mqtt_host"))
        strlcpy(s_data.mqtt_host, obj["mqtt_host"], sizeof(s_data.mqtt_host));
      s_data.mqtt_port = obj["mqtt_port"] | s_data.mqtt_port;
      if (obj.containsKey("mqtt_topic"))
        strlcpy(s_data.mqtt_topic, obj["mqtt_topic"], sizeof(s_data.mqtt_topic));

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
  obj["refresh_interval_s"] = (uint32_t)s_data.refresh_interval_s;
  obj["inactivity_timeout_s"] = (uint32_t)s_data.inactivity_timeout_s;
  obj["history_retention_d"] = (uint32_t)s_data.history_retention_d;
  obj["wifi_ssid"] = s_data.wifi_ssid;
  obj["wifi_password"] = s_data.wifi_password;
  obj["mqtt_host"] = s_data.mqtt_host;
  obj["mqtt_port"] = s_data.mqtt_port;
  obj["mqtt_topic"] = s_data.mqtt_topic;

  String output;
  serializeJson(doc, output);
  return sd_storage::saveConfig(output.c_str(), output.length());
}

} // namespace settings
