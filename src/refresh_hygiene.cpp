#include <Arduino.h>
#include <time.h>
#include "config.h"
#include "refresh_hygiene.h"

namespace refresh_hygiene {

// Survives deep sleep (same RTC-memory technique networking.cpp and
// battery.cpp use) so the absolute backstop spans sleep→wake cycles.
RTC_DATA_ATTR static time_t s_lastFlashEpoch = 0;

static int s_credits = 0;
static unsigned long s_sessionStartMs = 0;

// Clock plausible (post-2023) — i.e. set by NTP/RTC, not the boot default.
static bool timeValid() {
  return time(nullptr) > 1700000000;
}

void begin() {
  s_sessionStartMs = millis();
  if (s_lastFlashEpoch == 0 && timeValid()) s_lastFlashEpoch = time(nullptr);
  Serial.printf("[hygiene] gray diff %s, scroll reflash every %d, credit cap %d\n",
                config::GRAY_DIFF_ENABLED ? "on" : "off",
                config::SCROLL_FLASH_EVERY, config::HYGIENE_CREDIT_CAP);
}

void markFullFlash() {
  s_credits = 0;
  if (timeValid()) s_lastFlashEpoch = time(nullptr);
  Serial.println("[hygiene] full flash — credits reset");
}

static void credit(int n, const char* kind) {
  s_credits += n;
  Serial.printf("[hygiene] +%d (%s) -> %d credits (cap %d)\n",
                n, kind, s_credits, config::HYGIENE_CREDIT_CAP);
}

void creditDiff(int n)  { credit(n, "diff"); }
void creditGhost(int n) { credit(n, "ghost"); }

bool wantsFlash() {
  if (s_credits >= config::HYGIENE_CREDIT_CAP) return true;
  // Time backstops require credits >= 1: a panel nobody drove needs no
  // reset — static e-paper frames are stable.
  if (s_credits >= 1
      && (millis() - s_sessionStartMs) >= config::HYGIENE_SESSION_MS) return true;
  if (s_credits >= 1 && timeValid()
      && (time(nullptr) - s_lastFlashEpoch) >= config::HYGIENE_ABSOLUTE_S) return true;
  return false;
}

int credits() { return s_credits; }

} // namespace refresh_hygiene
