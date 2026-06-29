#pragma once
#include <cstdint>

namespace touch_input {

bool begin();
bool poll(int16_t &x, int16_t &y);
void sleep();
bool isOnline();

// Fast check: is a finger currently on the panel? (INT pin, no I2C read)
bool isTouched();

} // namespace touch_input
