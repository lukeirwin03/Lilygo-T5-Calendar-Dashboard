#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "config.h"
#include "display_manager.h"
#include "dashboards/calendar_dashboard.h"
#include "ui.h"
#include "networking.h"
#include "power_mgr.h"
#include "touch_input.h"
#include "battery.h"
#include "settings.h"
#include "sd_storage.h"

// ---------------------------------------------------------------------------
// Dashboard registry — CalendarDashboard handles JSON parsing only.
// Rendering and touch are handled by ui.
// ---------------------------------------------------------------------------
static CalendarDashboard calendarDash;

Dashboard* dashboards[] = {
  &calendarDash
};
size_t NUM_DASHBOARDS = sizeof(dashboards) / sizeof(dashboards[0]);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static unsigned long lastActivityMs = 0;
static unsigned long lastHeartbeatMs = 0;
static bool needsFullRender = true;
static unsigned long lastButtonMs = 0;
// Earliest plausible epoch — used to detect an unset clock (pre-NTP).
static constexpr time_t MIN_REASONABLE_EPOCH = 1700000000;  // 2023-11-14

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void logBootInfo() {
  Serial.println("\n========================================");
  Serial.println("  T5 Calendar Dashboard");
  Serial.println("========================================");

  power_mgr::WakeReason wake = power_mgr::currentWakeReason();
  const char* wakeName = "?";
  switch (wake) {
    case power_mgr::WAKE_COLD_BOOT: wakeName = "COLD_BOOT"; break;
    case power_mgr::WAKE_TIMER:     wakeName = "TIMER";     break;
    case power_mgr::WAKE_BUTTON:    wakeName = "BUTTON";    break;
    case power_mgr::WAKE_TOUCH:     wakeName = "TOUCH";     break;
  }
  Serial.printf("Wake reason: %s\n", wakeName);
  Serial.printf("Free heap:   %u\n", ESP.getFreeHeap());
  Serial.printf("PSRAM:       %s (%u bytes)\n",
                psramFound() ? "yes" : "no", ESP.getPsramSize());
  Serial.printf("Flash size:  %u MB\n", ESP.getFlashChipSize() / (1024*1024));

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  Serial.printf("RTC time:    %04d-%02d-%02d %02d:%02d:%02d\n",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                lt.tm_hour, lt.tm_min, lt.tm_sec);
  Serial.println("----------------------------------------\n");
}

// Feed parsed events from CalendarDashboard into ui for rendering.
static void syncEventsToUI() {
  if (calendarDash.hasData) {
    ui::setEvents(calendarDash.events(), calendarDash.eventCount());
  }
}

static void doRender() {
  // Always consume the UI pending flag so a full render doesn't leave a
  // stale partial-refresh request for the next loop iteration.
  bool uiPending = ui::needsRender();
  if (!needsFullRender && !uiPending) return;

  if (needsFullRender) {
    // Full refresh — screen change, wake, or new data. Discard any pending
    // partial mode so it can't leak into the next render.
    ui::refreshMode();
    display_mgr::powerOn();
    epd_clear();
    ui::render();
    display_mgr::fullRefresh();
    display_mgr::powerOff();
    Serial.println("[render] Full refresh");
  } else {
    // Check for partial refresh mode
    int mode = ui::refreshMode();
    if (mode == 2) {  // REFRESH_PARTIAL_DAILY
      ui::render();
      int dx, dy, dw, dh;
      ui::getDailyDirtyRect(dx, dy, dw, dh);
      display_mgr::partialRefresh(dx, dy, dw, dh);
      Serial.println("[render] Partial refresh (daily)");
    } else if (mode == 1) {  // REFRESH_PARTIAL_SETTINGS (modal)
      ui::render();
      int sx, sy, sw, sh;
      ui::getSettingsDirtyRect(sx, sy, sw, sh);
      display_mgr::partialRefresh(sx, sy, sw, sh);
      Serial.println("[render] Partial refresh (settings)");
    } else {
      display_mgr::powerOn();
      epd_clear();
      ui::render();
      display_mgr::fullRefresh();
      display_mgr::powerOff();
      Serial.println("[render] Full refresh");
    }
  }

  needsFullRender = false;
  calendarDash.dirty = false;  // a render consumes any pending "new data" flag
}

static void handleTouch() {
  bool touched = touch_input::isTouched();
  int16_t x = 0, y = 0;
  if (touched) {
    touch_input::poll(x, y);
    lastActivityMs = millis();
  }
  ui::updateTouch(touched, x, y);
}

// True if `lt` falls inside the nightly sleep window, which spans midnight
// (e.g. 22:00 -> 07:00). An inverted/misconfigured window (start <= end) is
// treated as "no scheduled sleep" so the device can't get stuck sleeping.
static bool isInSleepWindow(const struct tm& lt) {
  const settings::Data& cfg = settings::get();
  return cfg.sleep_start_hour > cfg.sleep_end_hour &&
         (lt.tm_hour >= cfg.sleep_start_hour || lt.tm_hour < cfg.sleep_end_hour);
}

// Milliseconds until the sleep window ends (e.g. until 7 AM). `lt` must be the
// local-time breakdown of `now`, and the window must be valid (start > end).
static unsigned long sleepUntilWindowEndMs(time_t now, const struct tm& lt) {
  const settings::Data& cfg = settings::get();
  struct tm target = lt;
  target.tm_hour = cfg.sleep_end_hour;
  target.tm_min = 0;
  target.tm_sec = 0;
  if (lt.tm_hour >= cfg.sleep_start_hour) target.tm_mday++;  // evening -> next morning
  time_t targetEpoch = mktime(&target);
  long diffSec = (long)difftime(targetEpoch, now);
  if (diffSec < 60) diffSec = 60;
  return (unsigned long)diffSec * 1000UL;
}

// Milliseconds until the sleep window starts (e.g. until 10 PM today).
// Caller must ensure a valid window (start > end) before calling.
static unsigned long msUntilWindowStart(time_t now, const struct tm& lt) {
  const settings::Data& cfg = settings::get();
  struct tm target = lt;
  target.tm_hour = cfg.sleep_start_hour;
  target.tm_min = 0;
  target.tm_sec = 0;
  time_t targetEpoch = mktime(&target);
  long diffSec = (long)difftime(targetEpoch, now);
  if (diffSec < 0) diffSec += 24 * 3600;  // window start already passed; it's tomorrow
  return (unsigned long)diffSec * 1000UL;
}

// Earliest start epoch of a future, non-all-day event whose scheduled wake-up
// (start minus lead time) is still ahead of `now`. Returns 0 if none.
// All-day events are skipped — they have no meaningful "about to start" moment
// and are caught by the periodic fallback.
static time_t nextEventStartAfter(time_t now) {
  time_t best = 0;
  for (int i = 0; i < calendarDash.eventCount(); i++) {
    const CalendarEvent& ev = calendarDash.events()[i];
    if (ev.allDay) continue;

    int y, m, d;
    if (sscanf(ev.date, "%d-%d-%d", &y, &m, &d) != 3) continue;
    struct tm etm;
    memset(&etm, 0, sizeof(etm));
    etm.tm_year  = y - 1900;
    etm.tm_mon   = m - 1;
    etm.tm_mday  = d;
    etm.tm_hour  = ev.startHour;
    etm.tm_min   = ev.startMin;
    etm.tm_sec   = 0;
    etm.tm_isdst = -1;  // let the system determine DST
    time_t start = mktime(&etm);

    // Only schedule a wake for events whose lead-time wake-up is still in the
    // future. Events already inside the lead window were either refreshed on a
    // prior wake or will be caught by the periodic cap.
    if (start - (time_t)config::EVENT_WAKE_LEAD_S <= now) continue;
    if (best == 0 || start < best) best = start;
  }
  return best;
}

static unsigned long calculateSleepMs() {
  time_t now = time(nullptr);
  const settings::Data& cfg = settings::get();

  // Clock not set (pre-NTP): can't do smart scheduling — use flat cap.
  if (now < MIN_REASONABLE_EPOCH) {
    return cfg.refresh_interval_s * 1000UL;
  }

  struct tm lt;
  localtime_r(&now, &lt);

  // Inside the nightly sleep window → sleep straight to morning.
  if (isInSleepWindow(lt)) {
    return sleepUntilWindowEndMs(now, lt);
  }

  // Fallback: the periodic cap guarantees we wake often enough to discover
  // events added to MQTT while we were asleep.
  unsigned long sleepMs = cfg.refresh_interval_s * 1000UL;

  // Event-aware: wake shortly before the next upcoming event so the display
  // is fresh when something is about to happen.
  time_t nextEv = nextEventStartAfter(now);
  if (nextEv > 0) {
    long diffSec = (long)difftime(nextEv, now) - (long)config::EVENT_WAKE_LEAD_S;
    if (diffSec < 60) diffSec = 60;  // clamp to avoid tight wake loops
    unsigned long eventMs = (unsigned long)diffSec * 1000UL;
    if (eventMs < sleepMs) sleepMs = eventMs;
  }

  // If the sleep window opens before our next planned wake, skip straight to
  // morning. This avoids a wasteful wake-then-resleep at the boundary, and
  // means events during sleep hours never wake the device.
  if (cfg.sleep_start_hour > cfg.sleep_end_hour) {
    unsigned long toWindowMs = msUntilWindowStart(now, lt);
    if (toWindowMs < sleepMs) {
      return sleepUntilWindowEndMs(now, lt);
    }
  }

  return sleepMs;
}

static void enterSleep(const char* reason) {
  unsigned long sleepMs = calculateSleepMs();
  Serial.printf("[sleep] Entering deep sleep (%s) for %lu ms\n", reason, sleepMs);
  touch_input::sleep();
  delay(50);
  power_mgr::sleepFor(sleepMs);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  setCpuFrequencyMhz(160);  // save ~30% active power vs default 240 MHz
  // The RTC keeps UTC across deep sleep, but the C library timezone state
  // (set by configTzTime) lives in RAM and is lost on wake. Re-apply the
  // timezone before any localtime_r call so sleep-window checks are correct.
  // NTP servers are passed later by connectWiFi().
  configTzTime(config::TIMEZONE, nullptr, nullptr);
  logBootInfo();

  if (!display_mgr::begin()) {
    Serial.println("[fatal] Display init failed — halting");
    while (1) delay(1000);
  }

  power_mgr::WakeReason wake = power_mgr::currentWakeReason();

  // Battery level changes slowly — only sample when someone might look at it
  // (cold boot or button wake). Skip on timer wakes to save ADC power.
  if (wake != power_mgr::WAKE_TIMER) {
    battery::sample();
  }
  touch_input::begin();
  pinMode(config::BUTTON_PIN, INPUT_PULLUP);
  lastButtonMs = millis();

  // SD card and settings must be initialized before using configurable values.
  sd_storage::begin();
  settings::init();

  // Trim old log files using the user-configurable retention window.
  sd_storage::cleanOldLogs((int)settings::get().history_retention_d);
  sd_storage::cleanOldHistory((int)settings::get().history_retention_d);

  // -------------------------------------------------------------------------
  // COLD BOOT → SD-first render, then WiFi/MQTT background refresh
  // -------------------------------------------------------------------------
  if (wake == power_mgr::WAKE_COLD_BOOT) {
    // --- SD-first: show cached data immediately if the RTC clock is valid ---
    bool renderedFromCache = false;
    if (time(nullptr) >= MIN_REASONABLE_EPOCH && networking::replaySDPayload()) {
      syncEventsToUI();
      needsFullRender = true;
      doRender();
      renderedFromCache = true;
      Serial.println("[boot] Rendered from SD cache");
    }

    if (!renderedFromCache) {
      // No usable cache or clock not set yet — show splash while connecting.
      display_mgr::drawSplash("Connecting...");
      display_mgr::powerOn();
      display_mgr::fullRefresh();
      display_mgr::powerOff();
    }

    // --- WiFi / NTP / MQTT refresh ---
    networking::connectWiFi();
    networking::waitForTimeSync(config::NTP_SYNC_TIMEOUT_MS);

    bool gotFreshData = false;
    if (networking::isWiFiConnected()) {
      networking::connectMqtt();
      // If we already rendered from cache, wait for NEW data specifically
      // (pumpForFreshPayload). Otherwise, wait for any data (pumpForPayload).
      if (renderedFromCache) {
        gotFreshData = networking::pumpForFreshPayload(config::PAYLOAD_WAIT_MS);
      } else {
        gotFreshData = networking::pumpForPayload(config::PAYLOAD_WAIT_MS);
      }
      // Disconnect WiFi as soon as we have the payload — the render and
      // interactive period don't need network, and WiFi is the biggest draw.
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[wifi] Disconnected to save power");
    }

    // If still no data (MQTT failed), fall back to caches (no WiFi needed).
    if (!calendarDash.hasData) {
      if (networking::hasCachedPayload()) {
        networking::replayCachedPayload();
      } else {
        networking::replaySDPayload();
      }
    }

    syncEventsToUI();

    // Re-render if we got fresh data, or haven't rendered yet.
    if (gotFreshData || !renderedFromCache) {
      needsFullRender = true;
    }

    lastActivityMs = millis();
  }

  // -------------------------------------------------------------------------
  // TIMER WAKE → quick data refresh then back to sleep
  // -------------------------------------------------------------------------
  else if (wake == power_mgr::WAKE_TIMER) {
    // If RTC says we're inside the sleep window, skip the expensive WiFi
    // refresh and sleep straight to morning. (RTC time is valid immediately
    // after deep sleep.)
    time_t tnow = time(nullptr);
    if (tnow >= MIN_REASONABLE_EPOCH) {
      struct tm wlt; localtime_r(&tnow, &wlt);
      if (isInSleepWindow(wlt)) {
        Serial.println("[sleep] Timer wake inside sleep window — skipping refresh");
        enterSleep("timer wake in sleep window");
      }
    }

    networking::connectWiFi();
    networking::waitForTimeSync(3000);

    if (networking::isWiFiConnected()) {
      networking::connectMqtt();
      if (networking::pumpForPayload(config::PAYLOAD_WAIT_MS)) {
        needsFullRender = true;
      }
      // Disconnect WiFi as soon as we have the payload — the render and sleep
      // don't need network, and WiFi is the single biggest power draw.
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[wifi] Disconnected after payload");
    }

    if (!calendarDash.hasData) {
      if (networking::hasCachedPayload()) {
        networking::replayCachedPayload();
      } else {
        networking::replaySDPayload();
      }
    }

    syncEventsToUI();
    doRender();
    enterSleep("timer wake done");
  }

  // -------------------------------------------------------------------------
  // BUTTON / TOUCH WAKE → replay cached data, stay awake for interaction
  // -------------------------------------------------------------------------
  else {
    if (networking::hasCachedPayload()) {
      networking::replayCachedPayload();
    } else {
      networking::replaySDPayload();
    }
    syncEventsToUI();
    needsFullRender = true;
    lastActivityMs = millis();
  }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  if (networking::isWiFiConnected()) {
    networking::loop();  // keep MQTT alive
  }
  handleTouch();

  // If new MQTT data arrived during networking::loop(), feed it to the UI
  if (calendarDash.dirty) {
    calendarDash.dirty = false;
    syncEventsToUI();
    needsFullRender = true;
  }

  // Button toggles the settings modal (active low, debounced)
  if (digitalRead(config::BUTTON_PIN) == LOW) {
    if (now - lastButtonMs > 300) {
      lastButtonMs = now;
      lastActivityMs = now;
      ui::toggleSettings();
      Serial.println("[button] Toggle settings");
    }
  }

  doRender();

  // Re-read the clock after rendering — the render takes seconds and
  // without a fresh timestamp the unsigned subtraction can underflow.
  now = millis();

  // Sleep decision: both the nightly window and the inactivity timeout are
  // gated on inactivity, so active use (touch/button) keeps the device awake
  // even after the window opens. The sleep *duration* (computed by
  // calculateSleepMs via enterSleep) decides whether to sleep until morning
  // (in window) or until the next event / periodic cap.
  unsigned long inactivityMs = settings::get().inactivity_timeout_s * 1000UL;
  if (now - lastActivityMs > inactivityMs) {
    const char* reason = "inactivity timeout";
    time_t tnow = time(nullptr);
    if (tnow >= MIN_REASONABLE_EPOCH) {
      struct tm lt; localtime_r(&tnow, &lt);
      if (isInSleepWindow(lt)) reason = "inactivity + sleep window";
    }
    ui::resetToDefaultView();
    doRender();
    enterSleep(reason);
  }

  if (now - lastHeartbeatMs > config::HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    networking::logHealth();
  }

  delay(10); // fast touch poll
}
