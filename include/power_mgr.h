#pragma once

namespace power_mgr {

enum WakeReason {
  WAKE_COLD_BOOT,
  WAKE_TIMER,
  WAKE_BUTTON,
  WAKE_TOUCH
};

WakeReason currentWakeReason();

// Sleep for intervalMs (or until button/touch wake).
// Never returns.
void sleepFor(unsigned long intervalMs);

} // namespace power_mgr
