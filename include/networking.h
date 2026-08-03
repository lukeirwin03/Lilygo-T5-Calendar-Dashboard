#pragma once

namespace networking {
  void connectWiFi();
  void connectMqtt();
  void loop();              // call mqtt.loop() — keep connection alive
  void logHealth();
  bool isWiFiConnected();

  // Blocks up to timeoutMs waiting for SNTP to set the system clock.
  // Returns true once time(nullptr) reflects a real epoch (either it
  // was already synced from a prior boot's RTC value, or SNTP completed
  // within the window). Returns false on timeout — the caller should
  // assume wake-from-sleep alignment will fall back to a flat 1-hour
  // interval until NTP can complete on a later boot.
  bool waitForTimeSync(unsigned long timeoutMs);

  // Pumps mqtt.loop() until either the retained payload has been received
  // (dashboards[0]->hasData becomes true) or timeoutMs has elapsed.
  // Returns true if a payload was received during this call.
  bool pumpForPayload(unsigned long timeoutMs);

  // Replay the last MQTT payload from RTC RAM into the dashboards.
  // Returns true if a cached payload existed and was successfully parsed.
  bool replayCachedPayload();

  // True if any payload is currently cached in RTC RAM.
  bool hasCachedPayload();

  // Dispatch a raw JSON payload through the dashboard pipeline.
  // Used for replaying the SD-cached payload on boot.
  bool dispatchPayload(const char* payload, size_t length);

  // Load and dispatch the SD-cached payload. Returns false if no cache
  // exists or the card isn't mounted.
  bool replaySDPayload();

  // Like pumpForPayload, but waits for a NEW message even if dashboards
  // already have cached data. Clears dirty flags first, then pumps until
  // a fresh payload arrives or timeout. Used on cold boot when cache was
  // already loaded from SD.
  bool pumpForFreshPayload(unsigned long timeoutMs);
}