#include "battery.h"
#include "config.h"
#include <Arduino.h>
#include <esp_attr.h>

namespace battery {

static RTC_DATA_ATTR int s_lastMv = -1;

void sample() {
  analogSetPinAttenuation(config::BATTERY_ADC_PIN, ADC_11db);
  (void)analogReadMilliVolts(config::BATTERY_ADC_PIN);

  uint32_t accum = 0;
  for (int i = 0; i < config::BATTERY_SAMPLES; i++) {
    accum += analogReadMilliVolts(config::BATTERY_ADC_PIN);
  }
  int pinMv = (int)(accum / config::BATTERY_SAMPLES);
  s_lastMv  = (int)(pinMv * config::BATTERY_DIVIDER);

  Serial.printf("[battery] %d mV at pin x %.2f = %d mV Vbat (~%d%%)\n",
                pinMv, config::BATTERY_DIVIDER, s_lastMv, lastPercent());
}

int lastMilliVolts() { return s_lastMv; }

int lastPercent() {
  int mv = s_lastMv;
  if (mv < 0) return -1;
  if (mv >= 4200) return 100;
  if (mv >= 4100) return 95 + (mv - 4100) *  5 / 100;
  if (mv >= 4000) return 85 + (mv - 4000) * 10 / 100;
  if (mv >= 3900) return 75 + (mv - 3900) * 10 / 100;
  if (mv >= 3800) return 55 + (mv - 3800) * 20 / 100;
  if (mv >= 3700) return 35 + (mv - 3700) * 20 / 100;
  if (mv >= 3600) return 20 + (mv - 3600) * 15 / 100;
  if (mv >= 3400) return  5 + (mv - 3400) * 15 / 200;
  if (mv >= 3300) return      (mv - 3300) *  5 / 100;
  return 0;
}

} // namespace battery
