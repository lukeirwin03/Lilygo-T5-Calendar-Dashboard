#include "networking.h"
#include "config.h"
#include "dashboard.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_attr.h>

extern Dashboard* dashboards[];
extern size_t NUM_DASHBOARDS;

namespace networking {

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

// Cached MQTT payload kept in RTC slow memory so it survives deep sleep.
// Must match the MQTT buffer size set in connectMqtt() — a payload larger
// than this cap is received and parsed live but silently not cached, so the
// next button-wake would have no data.
static constexpr size_t RTC_PAYLOAD_CAP = 4096;
RTC_DATA_ATTR static char  rtcPayload[RTC_PAYLOAD_CAP];
RTC_DATA_ATTR static size_t rtcPayloadLen = 0;

// -- Helpers ------------------------------------------------------------------

static const char* mqttStateName(int state) {
  switch (state) {
    case -4: return "CONNECTION_TIMEOUT";
    case -3: return "CONNECTION_LOST";
    case -2: return "CONNECT_FAILED";
    case -1: return "DISCONNECTED";
    case  0: return "CONNECTED";
    case  1: return "BAD_PROTOCOL";
    case  2: return "BAD_CLIENT_ID";
    case  3: return "UNAVAILABLE";
    case  4: return "BAD_CREDENTIALS";
    case  5: return "UNAUTHORIZED";
    default: return "UNKNOWN";
  }
}

static void dispatchDoc(const char* topic, JsonDocument& doc) {
  bool dispatched = false;
  for (size_t i = 0; i < NUM_DASHBOARDS; i++) {
    if (strcmp(topic, dashboards[i]->topic()) == 0) {
      dashboards[i]->handlePayload(doc);
      Serial.printf("[mqtt] Dispatched to: %s\n", dashboards[i]->name());
      dispatched = true;
    }
  }
  if (!dispatched) {
    Serial.printf("[mqtt] No handler for topic: %s\n", topic);
  }
}

static void onMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[mqtt] === MESSAGE RECEIVED ===\n");
  Serial.printf("[mqtt] Topic: %s\n", topic);
  Serial.printf("[mqtt] Length: %u bytes\n", length);

  // Print first 200 chars of payload for debugging
  char preview[201];
  size_t previewLen = length > 200 ? 200 : length;
  memcpy(preview, payload, previewLen);
  preview[previewLen] = '\0';
  Serial.printf("[mqtt] Payload preview: %s\n", preview);

  // Cache to RTC RAM so a button-wake can re-render without hitting WiFi.
  if (length < RTC_PAYLOAD_CAP) {
    memcpy(rtcPayload, payload, length);
    rtcPayloadLen = length;
    rtcPayload[length] = '\0'; // null-terminate for safety
    Serial.printf("[mqtt] Cached to RTC RAM (%u bytes)\n", (unsigned)length);
  } else {
    Serial.printf("[mqtt] Payload %u B > RTC cache %u B — not cached\n",
                  length, (unsigned)RTC_PAYLOAD_CAP);
    rtcPayloadLen = 0;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[mqtt] JSON parse error: %s\n", err.c_str());
    return;
  }

  // Debug: show what keys we got
  Serial.printf("[mqtt] JSON keys: ");
  JsonObject root = doc.as<JsonObject>();
  bool first = true;
  for (JsonPair kv : root) {
    if (!first) Serial.print(", ");
    Serial.printf("%s", kv.key().c_str());
    first = false;
  }
  Serial.println();

  // Check for events array
  JsonArray arr = doc["events"];
  if (!arr.isNull()) {
    Serial.printf("[mqtt] Events array has %u items\n", arr.size());
  } else {
    Serial.println("[mqtt] WARNING: no 'events' array in payload!");
  }

  dispatchDoc(topic, doc);
  Serial.println("[mqtt] === END MESSAGE ===");
}

// -- Public API ---------------------------------------------------------------

void connectWiFi() {
  Serial.printf("[wifi] Connecting to %s...\n", config::WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config::WIFI_SSID, config::WIFI_PASSWORD);

  int attempts = 0;
  constexpr int MAX_WIFI_ATTEMPTS = 2;
  while (WiFi.status() != WL_CONNECTED && attempts < MAX_WIFI_ATTEMPTS) {
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
      if (millis() - start > 15000) break;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n[wifi] Timeout — restarting attempt");
      WiFi.disconnect(true);
      delay(1000);
      WiFi.begin(config::WIFI_SSID, config::WIFI_PASSWORD);
      attempts++;
    }
  }

  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] FAILED to connect");
    return;
  }
  Serial.println("[wifi] Connected");
  Serial.printf("  SSID:    %s\n", WiFi.SSID().c_str());
  Serial.printf("  IP:      %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("  DNS:     %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("  Subnet:  %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("  RSSI:    %d dBm\n", WiFi.RSSI());
  Serial.printf("  MAC:     %s\n", WiFi.macAddress().c_str());
  Serial.printf("  Channel: %d\n", WiFi.channel());

  // Kick off SNTP and apply the POSIX timezone. SNTP runs in the background;
  // time(nullptr) will start returning a real epoch within a few seconds.
  // waitForTimeSync() must be called before sleeping or the very first
  // scheduled wake will land at a random minute, not on the hour.
  configTzTime(config::TIMEZONE,
               config::NTP_SERVER_1,
               config::NTP_SERVER_2);
  Serial.printf("[time] NTP requested (servers %s, %s), TZ=%s\n",
                config::NTP_SERVER_1, config::NTP_SERVER_2, config::TIMEZONE);
}

bool waitForTimeSync(unsigned long timeoutMs) {
  constexpr time_t MIN_REASONABLE_EPOCH = 1700000000;  // 2023-11-14

  time_t now = time(nullptr);
  if (now >= MIN_REASONABLE_EPOCH) {
    struct tm lt;
    localtime_r(&now, &lt);
    Serial.printf("[time] Clock already set: %04d-%02d-%02d %02d:%02d:%02d\n",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return true;
  }

  Serial.printf("[time] Waiting for SNTP sync (up to %lu ms)...\n", timeoutMs);
  const unsigned long start = millis();
  while ((now = time(nullptr)) < MIN_REASONABLE_EPOCH) {
    if (millis() - start > timeoutMs) {
      Serial.println("[time] SNTP sync TIMEOUT — wake alignment will drift");
      return false;
    }
    delay(200);
  }

  struct tm lt;
  localtime_r(&now, &lt);
  Serial.printf("[time] Synced in %lu ms: %04d-%02d-%02d %02d:%02d:%02d\n",
                millis() - start,
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                lt.tm_hour, lt.tm_min, lt.tm_sec);
  return true;
}

void connectMqtt() {
  mqtt.setServer(config::MQTT_HOST, config::MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(4096);
  mqtt.setKeepAlive(30);

  Serial.printf("[mqtt] Setup complete. Server=%s:%d, ClientID=%s\n",
                config::MQTT_HOST, config::MQTT_PORT, config::MQTT_CLIENT_ID);

  int attempts = 0;
  constexpr int MAX_MQTT_ATTEMPTS = 5;
  while (!mqtt.connected() && attempts < MAX_MQTT_ATTEMPTS) {
    Serial.printf("[mqtt] Connecting to %s:%d as %s...\n",
                  config::MQTT_HOST, config::MQTT_PORT, config::MQTT_CLIENT_ID);

    bool ok;
    if (config::MQTT_USER && config::MQTT_USER[0] != '\0') {
      Serial.printf("[mqtt] Using auth (user=%s)\n", config::MQTT_USER);
      ok = mqtt.connect(config::MQTT_CLIENT_ID, config::MQTT_USER, config::MQTT_PASS);
    } else {
      Serial.println("[mqtt] No auth (anonymous)");
      ok = mqtt.connect(config::MQTT_CLIENT_ID);
    }

    if (ok) {
      Serial.println("[mqtt] Connected successfully!");
      for (size_t i = 0; i < NUM_DASHBOARDS; i++) {
        const char* topic = dashboards[i]->topic();
        bool subOk = mqtt.subscribe(topic);
        Serial.printf("[mqtt]   subscribe [%s] -> %s\n", topic, subOk ? "OK" : "FAIL");
      }
      return;
    } else {
      int s = mqtt.state();
      Serial.printf("[mqtt] Failed (state=%d %s) — retry in 2s\n",
                    s, mqttStateName(s));
      delay(2000);
      attempts++;
    }
  }
  if (!mqtt.connected()) {
    Serial.println("[mqtt] FAILED to connect — proceeding without fresh data");
  }
}

bool pumpForPayload(unsigned long timeoutMs) {
  Serial.printf("[mqtt] Pumping for payload (timeout=%lu ms)...\n", timeoutMs);
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    mqtt.loop();
    // Subscribers all share the same topic; the first dashboard that has
    // data after subscribe is a sufficient signal that the retained
    // message arrived.
    for (size_t i = 0; i < NUM_DASHBOARDS; i++) {
      if (dashboards[i]->hasData) {
        Serial.printf("[mqtt] Dashboard '%s' has data after %lu ms\n",
                      dashboards[i]->name(), millis() - start);
        return true;
      }
    }
    delay(20);
  }
  Serial.printf("[mqtt] Timeout after %lu ms — no payload received\n", millis() - start);
  return false;
}

bool replayCachedPayload() {
  if (rtcPayloadLen == 0) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, rtcPayload, rtcPayloadLen);
  if (err) {
    Serial.printf("[mqtt] Cached payload parse error: %s\n", err.c_str());
    return false;
  }

  // All dashboards share the same topic in this firmware. Dispatch by
  // walking the registry and matching topics — same logic as live MQTT.
  // Use the first dashboard's topic as the canonical key.
  if (NUM_DASHBOARDS == 0) return false;
  const char* topic = dashboards[0]->topic();
  dispatchDoc(topic, doc);
  Serial.printf("[mqtt] Replayed %u cached bytes\n", (unsigned)rtcPayloadLen);
  return true;
}

bool hasCachedPayload() { return rtcPayloadLen > 0; }

void logHealth() {
  Serial.println("---- health check ----");
  Serial.printf("  Uptime:      %lu s\n", millis() / 1000);
  Serial.printf("  Free heap:   %u bytes\n", ESP.getFreeHeap());
  Serial.printf("  WiFi:        %s (RSSI %d dBm)\n",
                WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
                WiFi.RSSI());
  Serial.printf("  MQTT:        %s (state=%s)\n",
                mqtt.connected() ? "UP" : "DOWN",
                mqttStateName(mqtt.state()));
  Serial.println("----------------------");
}

void loop() {
  if (mqtt.connected()) {
    mqtt.loop();
  }
}

bool isWiFiConnected() { return WiFi.status() == WL_CONNECTED; }

} // namespace networking