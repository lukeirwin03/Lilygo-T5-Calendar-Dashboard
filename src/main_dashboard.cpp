#include <Arduino.h>
#include <WiFi.h>
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
  // Check if ui has pending changes
  if (!needsFullRender && !ui::needsRender()) return;

  if (needsFullRender) {
    // Full refresh — screen change, wake, or new data
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

static unsigned long calculateSleepMs() {
  time_t now = time(nullptr);
  constexpr time_t MIN_REASONABLE_EPOCH = 1700000000; // 2023-11-14
  if (now < MIN_REASONABLE_EPOCH) {
    return settings::get().refresh_interval_s * 1000UL;
  }
  struct tm lt; localtime_r(&now, &lt);
  if (lt.tm_hour >= 22 || lt.tm_hour < 7) {
    struct tm target = lt;
    target.tm_hour = 7; target.tm_min = 0; target.tm_sec = 0;
    if (lt.tm_hour >= 22) target.tm_mday++;
    time_t targetEpoch = mktime(&target);
    long diffSec = (long)difftime(targetEpoch, now);
    if (diffSec < 60) diffSec = 60;
    return (unsigned long)diffSec * 1000UL;
  }
  return settings::get().refresh_interval_s * 1000UL;
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
  logBootInfo();

  if (!display_mgr::begin()) {
    Serial.println("[fatal] Display init failed — halting");
    while (1) delay(1000);
  }

  battery::sample();
  touch_input::begin();
  pinMode(config::BUTTON_PIN, INPUT_PULLUP);
  lastButtonMs = millis();

  // SD card and settings must be initialized before using configurable values.
  sd_storage::begin();
  settings::init();

  // Trim old log files using the user-configurable retention window.
  sd_storage::cleanOldLogs((int)settings::get().history_retention_d);

  power_mgr::WakeReason wake = power_mgr::currentWakeReason();

  // -------------------------------------------------------------------------
  // COLD BOOT → connect WiFi, MQTT, fetch fresh data, render
  // -------------------------------------------------------------------------
  if (wake == power_mgr::WAKE_COLD_BOOT) {
    display_mgr::drawSplash("Connecting...");
    display_mgr::powerOn();
    display_mgr::fullRefresh();
    display_mgr::powerOff();

    networking::connectWiFi();
    networking::waitForTimeSync(config::NTP_SYNC_TIMEOUT_MS);

    if (networking::isWiFiConnected()) {
      networking::connectMqtt();
      networking::pumpForPayload(config::PAYLOAD_WAIT_MS);
    }

    // If MQTT failed or no data arrived, try RTC cache
    if (!calendarDash.hasData && networking::hasCachedPayload()) {
      networking::replayCachedPayload();
    }

    syncEventsToUI();

    // Disconnect WiFi to save power during interactive period.
    // The retained MQTT payload is already cached; scheduled wakes
    // will reconnect.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] Disconnected to save power");

    needsFullRender = true;
    lastActivityMs = millis();
  }

  // -------------------------------------------------------------------------
  // TIMER WAKE → quick data refresh then back to sleep
  // -------------------------------------------------------------------------
  else if (wake == power_mgr::WAKE_TIMER) {
    networking::connectWiFi();
    networking::waitForTimeSync(3000);

    if (networking::isWiFiConnected()) {
      networking::connectMqtt();
      if (networking::pumpForPayload(config::PAYLOAD_WAIT_MS)) {
        needsFullRender = true;
      }
    }

    if (!calendarDash.hasData && networking::hasCachedPayload()) {
      networking::replayCachedPayload();
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

  // Button cycles between weekly and daily views (active low, debounced)
  if (digitalRead(config::BUTTON_PIN) == LOW) {
    if (now - lastButtonMs > 300) {
      lastButtonMs = now;
      lastActivityMs = now;
      ui::next();
      needsFullRender = true;
      Serial.println("[button] Toggle view");
    }
  }

  doRender();

  // Re-read the clock after rendering — the render takes seconds and
  // without a fresh timestamp the unsigned subtraction can underflow.
  now = millis();

  // Sleep logic: outside active hours (7am-10pm) sleep until 7am
  time_t tnow = time(nullptr);
  constexpr time_t MIN_REASONABLE_EPOCH = 1700000000;
  if (tnow >= MIN_REASONABLE_EPOCH) {
    struct tm lt; localtime_r(&tnow, &lt);
    if (lt.tm_hour >= 22 || lt.tm_hour < 7) {
      enterSleep("outside active hours");
    }
  }

  if (now - lastActivityMs > config::INACTIVITY_TIMEOUT_MS) {
    enterSleep("inactivity timeout");
  }

  if (now - lastHeartbeatMs > config::HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    networking::logHealth();
  }

  delay(10); // fast touch poll
}
