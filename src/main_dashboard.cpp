#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "config.h"
#include "display_manager.h"
#include "dashboards/calendar_dashboard.h"
#include "ui.h"
#include "ui_settings.h"
#include "networking.h"
#include "conn_screen.h"
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
// Last touch/button input. Feeds the inactivity check in loop().
static unsigned long lastInputMs = 0;
// Guaranteed-awake floor, (re)set after any render that completes following
// a refresh (the setup wake paths set it after their final render; loop()
// resets it whenever a render runs): the device stays touch-responsive at
// least this long even with no input, so the multi-second render itself
// can't eat the window. Sleeps only after the floor passes AND the
// inactivity timeout has expired with no input. (lastInputMs starts at 0,
// so with no input the device sleeps at the floor once millis() exceeds the
// timeout — i.e. between 15 s and one timeout after the render completes.)
static unsigned long minAwakeUntilMs = 0;
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
    ui::setLastUpdated(calendarDash.lastUpdated());
  }
}

// Renders if anything is pending; returns true when a render was performed.
static bool doRender() {
  // Always consume the UI pending flag so a full render doesn't leave a
  // stale partial-refresh request for the next loop iteration.
  bool uiPending = ui::needsRender();
  if (!needsFullRender && !uiPending) return false;

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
      if (!ui_settings::diffPush()) {
        int sx, sy, sw, sh;
        ui::getSettingsDirtyRect(sx, sy, sw, sh);
        display_mgr::partialRefresh(sx, sy, sw, sh);
      }
      Serial.println("[render] Diff update (settings)");
    } else if (mode == 3) {  // REFRESH_PARTIAL_FOCUS (timeline scroll, ghost)
      ui::render();
      int fx, fy, fw, fh;
      ui::getFocusGhostRect(fx, fy, fw, fh);
      display_mgr::ghostRefresh(fx, fy, fw, fh);
      Serial.println("[render] Ghost refresh (focus timeline)");
    } else if (mode == 4) {  // REFRESH_SETTINGS_CLOSE (modal closing, cleared reflash)
      ui::render();
      ui_settings::closeReflash();
      Serial.println("[render] Reflash (settings close)");
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
  return true;
}

static void handleTouch() {
  bool touched = touch_input::isTouched();
  int16_t x = 0, y = 0;
  if (touched) {
    touch_input::poll(x, y);
    lastInputMs = millis();
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

// Bridge networking's progress events to the connection screen. The
// numeric stages line up with conn_screen's stage indices (see
// conn_screen.h).
static void connProgressCb(void* /*ctx*/, networking::ProgressStage stage,
                           networking::ProgressEvent event) {
  switch (event) {
    case networking::PROG_STARTED:   conn_screen::stageStart((int)stage); break;
    case networking::PROG_WAITING:   conn_screen::tick();                 break;
    case networking::PROG_DONE_OK:   conn_screen::stageDone((int)stage, true);  break;
    case networking::PROG_DONE_FAIL: conn_screen::stageDone((int)stage, false); break;
  }
}

// Connect WiFi/MQTT, pull the latest retained payload, disconnect.
// freshOnly=true waits for a message arriving during this call (used when a
// cached view is already on screen); false accepts any data. Returns true if
// a payload arrived. WiFi is always disconnected before returning.
static bool backgroundRefresh(bool freshOnly) {
  networking::connectWiFi();
  networking::waitForTimeSync(config::NTP_SYNC_TIMEOUT_MS);
  bool got = false;
  if (networking::isWiFiConnected()) {
    networking::connectMqtt();
    got = freshOnly ? networking::pumpForFreshPayload(config::PAYLOAD_WAIT_MS)
                    : networking::pumpForPayload(config::PAYLOAD_WAIT_MS);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] Disconnected to save power");
  }
  return got;
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

  // Periodic wake aligned to the interval boundary measured from the top of
  // the hour (hourly -> wakes at :00:10 each hour; 30 min -> :00/:30), so
  // updates happen at a predictable wall-clock time. +10s lands just past
  // the boundary so the retained message is freshly published.
  unsigned long intervalS = cfg.refresh_interval_s;
  if (intervalS < 60) intervalS = 60;  // safety clamp
  long intoHourS = (long)lt.tm_min * 60L + (long)lt.tm_sec;
  long intervalL = (long)intervalS;
  long toBoundaryS = intervalL - (intoHourS % intervalL) + 10;
  if (toBoundaryS < 60) toBoundaryS += intervalL;  // no tight wake loop at the boundary
  unsigned long sleepMs = (unsigned long)toBoundaryS * 1000UL;

  // If the sleep window opens before our next planned wake, skip straight to
  // morning. This avoids a wasteful wake-then-resleep at the boundary.
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
  sd_storage::cleanOldCache(config::CACHE_RETENTION_DAYS);

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

    bool gotFreshData = false;
    if (!renderedFromCache) {
      // No usable cache or clock not set yet — show the connection
      // screen with live partial-update progress, then connect. The
      // listener is scoped to this call so later refreshes (Sync button,
      // wakes with a view already on screen) never clobber the display.
      conn_screen::begin();
      networking::setProgressListener(connProgressCb);
      gotFreshData = backgroundRefresh(false);
      networking::setProgressListener(nullptr);
    } else {
      // Cache already on screen: only genuinely new data triggers a
      // re-render (freshOnly=true).
      gotFreshData = backgroundRefresh(true);
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

    minAwakeUntilMs = millis() + config::POST_REFRESH_AWAKE_MS;
  }

  // -------------------------------------------------------------------------
  // TIMER WAKE → refresh from MQTT, render, then stay briefly interactive
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

    if (backgroundRefresh(false)) {
      needsFullRender = true;
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
    minAwakeUntilMs = millis() + config::POST_REFRESH_AWAKE_MS;
    // Fall through to loop(): the device stays touch-responsive for the
    // post-refresh floor, then sleeps via the inactivity timeout.
  }

  // -------------------------------------------------------------------------
  // BUTTON / TOUCH WAKE → instant cache view, then background data refresh
  // -------------------------------------------------------------------------
  else {
    // Instant view from cache, then pull fresh data in the background
    // (button wake = force update). Re-render happens in loop() when the
    // fresh payload lands.
    if (networking::hasCachedPayload()) networking::replayCachedPayload();
    else networking::replaySDPayload();
    syncEventsToUI();
    needsFullRender = true;
    doRender();
    if (backgroundRefresh(true)) needsFullRender = true;
    syncEventsToUI();
    // The user woke the device to browse — the blocking refresh shouldn't
    // count against their inactivity budget.
    lastInputMs = millis();
    minAwakeUntilMs = millis() + config::POST_REFRESH_AWAKE_MS;
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

  // If the user changed Context Days in the Settings modal, reload the cached
  // payload so the new +/-N window takes effect immediately (no WiFi needed --
  // re-parse whatever we already have cached).
  if (ui::consumeEventReloadRequest()) {
    bool ok = networking::replayCachedPayload();   // re-parse RTC-cached payload with new window
    if (!ok) networking::replaySDPayload();         // fallback: re-parse the SD-cached payload
    syncEventsToUI();
    needsFullRender = true;
  }

  // "Sync" button in the settings modal: force a fresh MQTT pull mid-session.
  if (ui::consumeManualRefreshRequest()) {
    backgroundRefresh(true);
    syncEventsToUI();
    lastInputMs = millis();  // treat the tap as input: don't let the blocking refresh eat the timeout
    minAwakeUntilMs = millis() + config::POST_REFRESH_AWAKE_MS;
  }

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
      lastInputMs = now;
      ui::toggleSettings();
      Serial.println("[button] Toggle settings");
    }
  }

  if (doRender()) {
    // A render just completed — restart the guaranteed-awake window so the
    // user gets the full floor of touch time on the fresh image.
    minAwakeUntilMs = millis() + config::POST_REFRESH_AWAKE_MS;
  }

  // Re-read the clock after rendering — the render takes seconds and
  // without a fresh timestamp the unsigned subtraction can underflow.
  now = millis();

  // Sleep decision: both the nightly window and the inactivity timeout are
  // gated on inactivity, so active use (touch/button) keeps the device awake
  // even after the window opens. The sleep *duration* (computed by
  // calculateSleepMs via enterSleep) decides whether to sleep until morning
  // (in window) or to the next interval boundary.
  unsigned long inactivityMs = settings::get().inactivity_timeout_s * 1000UL;
  if (now >= minAwakeUntilMs && now - lastInputMs >= inactivityMs) {
    const char* reason = "inactivity timeout";
    time_t tnow = time(nullptr);
    if (tnow >= MIN_REASONABLE_EPOCH) {
      struct tm lt; localtime_r(&tnow, &lt);
      if (isInSleepWindow(lt)) reason = "inactivity + sleep window";
    }
    ui::resetToDefaultView();
    doRender();
    // A touch landing during the multi-second goodnight render would be
    // invisible to the sleep decision — re-check and stay awake if touched.
    if (touch_input::isTouched()) {
      lastInputMs = millis();
    } else {
      enterSleep(reason);
    }
  }

  if (now - lastHeartbeatMs > config::HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    networking::logHealth();
  }

  delay(10); // fast touch poll
}
