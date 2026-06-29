#pragma once
#include <ArduinoJson.h>

class Dashboard {
public:
  virtual ~Dashboard() {}
  virtual const char* topic() const = 0;
  virtual const char* name() const = 0;
  virtual void handlePayload(JsonDocument& doc) = 0;
  virtual void render() = 0;

  bool dirty   = false;
  bool hasData = false;
};