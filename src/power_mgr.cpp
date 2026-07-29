#include "power_mgr.h"
#include "config.h"
#include "display_manager.h"
#include <Arduino.h>
#include <esp_sleep.h>

namespace power_mgr {

WakeReason currentWakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: return WAKE_TIMER;
    case ESP_SLEEP_WAKEUP_EXT1:  {
      uint64_t gpio = esp_sleep_get_ext1_wakeup_status();
      if (gpio & (1ULL << config::TOUCH_WAKE_PIN)) return WAKE_TOUCH;
      if (gpio & (1ULL << config::BUTTON_PIN)) return WAKE_BUTTON;
      Serial.printf("[power] Unknown ext1 wake GPIO mask: 0x%llx\n", gpio);
      return WAKE_BUTTON;
    }
    default: return WAKE_COLD_BOOT;
  }
}

void sleepFor(unsigned long intervalMs) {
  Serial.printf("[power] Deep sleep for %lu ms\n", intervalMs);
  display_mgr::powerOff();

  esp_sleep_enable_timer_wakeup((uint64_t)intervalMs * 1000ULL);

  uint64_t ext1_mask = (1ULL << config::BUTTON_PIN);
  // NOTE: TOUCH_INT (GPIO47) is NOT RTC-capable on ESP32-S3.
  // To wake on touch, bridge TOUCH_INT to TOUCH_WAKE_PIN (GPIO10) and
  // uncomment the line below:
  // ext1_mask |= (1ULL << config::TOUCH_WAKE_PIN);
  esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.flush();
  esp_deep_sleep_start();
}

} // namespace power_mgr
