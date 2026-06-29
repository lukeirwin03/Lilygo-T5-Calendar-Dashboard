#include "touch_input.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrvGT911.hpp>
#include "epd_driver.h"
#include <esp_sleep.h>

static TouchDrvGT911 touch;
static bool online = false;
static uint8_t address = 0;

namespace touch_input {

bool begin() {
  Wire.begin(config::TOUCH_SDA, config::TOUCH_SCL);

  // After deep sleep the GT911 needs ~1s before it can be addressed reliably.
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
    delay(1000);
  }

  // Wake touch controller if it was sleeping
  pinMode(config::TOUCH_INT_PIN, OUTPUT);
  digitalWrite(config::TOUCH_INT_PIN, HIGH);

  Wire.beginTransmission(0x14);
  if (Wire.endTransmission() == 0) address = 0x14;
  else {
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) address = 0x5D;
  }

  if (address == 0) {
    Serial.println("[touch] GT911 not found");
    return false;
  }

  touch.setPins(-1, config::TOUCH_INT_PIN);
  if (!touch.begin(Wire, address, config::TOUCH_SDA, config::TOUCH_SCL)) {
    Serial.println("[touch] GT911 init failed");
    return false;
  }

  touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
  touch.setSwapXY(true);
  touch.setMirrorXY(false, true);
  online = true;
  Serial.printf("[touch] GT911 online @ 0x%02X\n", address);
  return true;
}

bool poll(int16_t &x, int16_t &y) {
  if (!online) return false;

  // Check INT pin first — GT911 drives it low when touch data is available.
  // This avoids unnecessary I2C reads and prevents clearing the point buffer
  // when no new data exists.
  if (digitalRead(config::TOUCH_INT_PIN) == HIGH) return false;

  uint8_t touched = touch.getPoint(&x, &y, 1);
  if (touched > 0) {
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
      Serial.printf("[touch] Out of bounds: x=%d y=%d\n", x, y);
      return false;
    }
    return true;
  }
  return false;
}

bool isTouched() {
  if (!online) return false;
  return digitalRead(config::TOUCH_INT_PIN) == LOW;
}

void sleep() {
  if (online) touch.sleep();
  Wire.end();
  pinMode(config::TOUCH_SDA, OPEN_DRAIN);
  pinMode(config::TOUCH_SCL, OPEN_DRAIN);
  pinMode(config::TOUCH_INT_PIN, OPEN_DRAIN);
}

bool isOnline() { return online; }

} // namespace touch_input
