#pragma once
#include <cstdint>

namespace battery {

// Read the divider on config::BATTERY_ADC_PIN, average BATTERY_SAMPLES
// readings, multiply by BATTERY_DIVIDER, and cache the result in RTC RAM
// so subsequent wakes can render before re-sampling.
void sample();

// Last cached reading. -1 == never sampled (e.g. very first cold boot
// before sample() ran). Survives deep sleep via RTC_DATA_ATTR.
int lastMilliVolts();

// 0..100 estimate from lastMilliVolts() via a piecewise-linear LiPo
// curve. Returns -1 if no sample yet. Clamps to [0, 100].
int lastPercent();

}
