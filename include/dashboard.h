#pragma once
#include <ArduinoJson.h>

class Dashboard {
public:
  virtual ~Dashboard() {}
  virtual const char* topic() const = 0;
  virtual const char* name() const = 0;
  virtual void handlePayload(JsonDocument& doc) = 0;
  virtual void render() = 0;

  // Persist the currently-parsed events to the day cache (no-op for dashboards
  // that don't keep one). Called by networking after a fresh payload is parsed.
  virtual void writeCacheFromCurrent() {}

  bool dirty   = false;
  bool hasData = false;
};