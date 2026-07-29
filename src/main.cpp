#include <Arduino.h>
#include "config.h"
#include "display_manager.h"
#include "ui.h"
#include "battery.h"
#include "touch_input.h"
#include "power_mgr.h"
#include "dashboards/calendar_dashboard.h"
#include "settings.h"
#include "sd_storage.h"

static unsigned long lastButtonMs = 0;
static unsigned long lastHeartbeatMs = 0;
static unsigned long lastActivityMs = 0;

static CalendarEvent testEvents[28];
static const int TEST_EVENT_COUNT = 26;

static void initTestEvents() {
  time_t now = time(nullptr);
  struct tm today;
  localtime_r(&now, &today);

  // Expanded test event definitions covering rendering edge cases.
  // Days 0-4 and 6 have events; Day 5 is intentionally empty to exercise
  // the "No events" placeholder.
  struct EvDef {
    const char* title; const char* loc; const char* desc;
    int dayOff; int hr, min, dur; bool allDay; uint8_t shade;
  } defs[] = {
    // ===== Day 0 (today) — boundary, pre-range, back-to-back =====
    { "Summer Art Festival",       "Aksarben Village",       "Final project deadline",            0,  0,  0,   0, true,   5 },
    { "Early morning jog",         "Neighborhood",           "",                                  0,  6, 30,  30, false,  8 },  // pre-7AM event
    { "Morning standup",           "Slack",                  "Daily team check-in",               0,  9,  0,  30, false,  8 },
    { "Client call",               "Zoom",                   "Quarterly review",                  0, 10,  0,  60, false,  5 },  // back-to-back with Team sync
    { "Team sync",                 "Conference Room",        "Sprint planning",                   0, 11,  0,  60, false, 12 },  // back-to-back with Client call
    { "Chiro",                     "West Omaha Chiro",       "Routine adjustment",                0, 15, 30,  30, false,  8 },

    // ===== Day 1 — triple overlap, long event, post-range =====
    { "Design review",             "Zoom",                   "Q3 design system review",           1, 10,  0,  90, false,  5 },  // triple overlap A
    { "Stakeholder sync",          "Zoom",                   "",                                  1, 10, 30,  60, false,  8 },  // triple overlap B
    { "Vendor call",               "Phone",                  "Contract negotiation",              1, 11,  0,  60, false, 12 },  // triple overlap C
    { "Lunch w/ team",             "Old Market",             "Birthday celebration for Sam",      1, 12,  0,  60, false, 12 },
    { "Deep work workshop",        "Conference Room A",      "Q3 roadmap planning session",       1, 14,  0, 240, false,  5 },  // 4-hour long event
    { "Late movie",                "Aksarben Theater",       "",                                  1, 22, 30, 120, false,  8 },  // post-10PM event

    // ===== Day 2 — very short, boundary end, multi-day start =====
    { "Tech Conference",           "Convention Center",      "Annual tech conference — all 3 days", 2,  0,  0,   0, true,   3 },  // multi-day: days 2, 3, 4
    { "Quick call",                "Slack",                  "5-minute check-in",                 2,  9,  0,  15, false,  8 },  // 15-min very short event
    { "Code review",               "Zoom",                   "Review dashboard PR",               2, 14,  0,  90, false,  5 },
    { "Late night deploy",         "",                       "Push v3.0 to production",           2, 22,  0,  60, false, 12 },  // exactly 10 PM: boundary end

    // ===== Day 3 — boundary start, long title, empty location =====
    { "Tech Conference",           "Convention Center",      "Annual tech conference — all 3 days", 3,  0,  0,   0, true,   3 },
    { "Early bird session",        "Room 101",               "Doors open at 7 AM sharp",          3,  7,  0,  30, false,  8 },  // exactly 7 AM: boundary start
    { "Doctor appointment at West Omaha Medical Center", "West Omaha Medical", "Annual physical", 3, 13,  0,  60, false,  8 },  // very long title
    { "Quick errand",              "",                       "",                                  3, 15,  0,  45, false, 12 },  // empty location + description

    // ===== Day 4 — multi-day ends, otherwise sparse =====
    { "Tech Conference",           "Convention Center",      "Annual tech conference — all 3 days", 4,  0,  0,   0, true,   3 },  // multi-day last day
    { "After-conference drinks",   "Aksarben",               "Casual networking",                 4, 17,  0,  90, false, 12 },

    // ===== Day 5 — completely empty (tests "No events" placeholder) =====
    // (no events defined)

    // ===== Day 6 — AM/PM crossing, multi-all-day same day =====
    { "Holiday Party",             "Office",                 "Annual celebration",                6,  0,  0,   0, true,   5 },  // all-day
    { "Company off-site",          "Zoo Pavilion",           "Team building",                     6,  0,  0,   0, true,  12 },  // second all-day same day
    { "Late lunch meeting",        "Aksarben",               "Cross-team sync",                   6, 11, 30,  90, false,  8 },  // 11:30 AM - 1:00 PM: AM/PM crossing
    { "Evening standup",           "Slack",                  "",                                  6, 16,  0,  30, false,  5 },
  };

  for (int i = 0; i < TEST_EVENT_COUNT; i++) {
    CalendarEvent& ev = testEvents[i];
    memset(&ev, 0, sizeof(ev));
    strlcpy(ev.title, defs[i].title, sizeof(ev.title));
    strlcpy(ev.location, defs[i].loc, sizeof(ev.location));
    strlcpy(ev.description, defs[i].desc, sizeof(ev.description));
    strlcpy(ev.calendar, "main", sizeof(ev.calendar));
    strlcpy(ev.type, "event", sizeof(ev.type));

    struct tm evDay = today;
    evDay.tm_mday += defs[i].dayOff;
    mktime(&evDay);
    snprintf(ev.date, sizeof(ev.date), "%04d-%02d-%02d",
             evDay.tm_year + 1900, evDay.tm_mon + 1, evDay.tm_mday);

    ev.startHour = defs[i].hr;
    ev.startMin = defs[i].min;
    ev.durationMin = defs[i].dur;
    ev.allDay = defs[i].allDay;
    ev.shade = defs[i].shade;
  }
}

static void logBootInfo() {
  Serial.println("\n========================================");
  Serial.println("  T5 Calendar Dashboard — UI DEMO MODE");
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
  Serial.println("----------------------------------------\n");
}

static void handleTouch() {
  bool touched = touch_input::isTouched();
  int16_t x = 0, y = 0;

  if (touched) {
    bool gotPoint = touch_input::poll(x, y);
    if (gotPoint) {
      lastActivityMs = millis();
      Serial.printf("[touch] x=%d y=%d\n", x, y);
    } else {
      // INT is LOW (touched=true) but GT911 returned no point data.
      // This indicates a touch controller communication issue.
      Serial.println("[touch] INT low but no point data");
      touched = false;
    }
  }

  ui::updateTouch(touched, x, y);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  logBootInfo();

  if (!display_mgr::begin()) {
    Serial.println("[fatal] Display init failed — halting");
    while (1) delay(1000);
  }

  battery::sample();
  if (!touch_input::begin()) {
    Serial.println("[ERROR] Touch init FAILED — touches will be ignored!");
  } else {
    Serial.println("[touch] GT911 initialized successfully");
  }
  pinMode(config::BUTTON_PIN, INPUT_PULLUP);
  lastButtonMs = millis();

  sd_storage::begin();
  settings::init();

  initTestEvents();
  ui::setEvents(testEvents, TEST_EVENT_COUNT);

  lastActivityMs = millis();
}

static void doRender() {
  int mode = ui::refreshMode();
  if (mode == 2) {  // REFRESH_PARTIAL_DAILY
    ui::render();
    int dx, dy, dw, dh;
    ui::getDailyDirtyRect(dx, dy, dw, dh);
    display_mgr::partialRefresh(dx, dy, dw, dh);
    Serial.println("[demo] Partial refresh (daily)");
  } else if (mode == 1) {  // REFRESH_PARTIAL_SETTINGS (modal)
    ui::render();
    int sx, sy, sw, sh;
    ui::getSettingsDirtyRect(sx, sy, sw, sh);
    display_mgr::partialRefresh(sx, sy, sw, sh);
    Serial.println("[demo] Partial refresh (settings)");
  } else {
    display_mgr::powerOn();
    epd_clear();
    ui::render();
    display_mgr::fullRefresh();
    display_mgr::powerOff();
    Serial.println("[demo] Rendered screen");
  }
}

static void enterSleep(const char* reason) {
  unsigned long sleepMs = settings::get().refresh_interval_s * 1000UL;
  Serial.printf("[demo sleep] %s — deep sleep for %lu ms\n", reason, sleepMs);
  touch_input::sleep();
  delay(50);
  power_mgr::sleepFor(sleepMs);  // never returns
}

void loop() {
  unsigned long now = millis();

  handleTouch();

  // Button toggles the settings modal (active low, debounced)
  if (digitalRead(config::BUTTON_PIN) == LOW) {
    if (now - lastButtonMs > 300) {
      lastButtonMs = now;
      lastActivityMs = now;
      ui::toggleSettings();
      Serial.println("[demo] Button -> toggle settings");
    }
  }

  // Render once if the UI has pending changes (from a touch gesture or button).
  // This consumes the UI pending flag exactly once, after all input is processed.
  if (ui::needsRender()) {
    doRender();
  }

  // Heartbeat every 5 seconds — shows touch controller status for debugging.
  if (now - lastHeartbeatMs > 5000) {
    lastHeartbeatMs = now;
    Serial.printf("[heartbeat] touch online=%d INT=%d heap=%u\n",
                  touch_input::isOnline(),
                  digitalRead(config::TOUCH_INT_PIN),
                  ESP.getFreeHeap());
  }

  // Re-read the clock after rendering (render takes seconds).
  now = millis();

  // Deep sleep on inactivity (demo has no RTC/NTP, so no active-hours gating).
  unsigned long inactivityMs = settings::get().inactivity_timeout_s * 1000UL;
  if (now - lastActivityMs > inactivityMs) {
    ui::resetToDefaultView();
    doRender();
    enterSleep("inactivity timeout");
  }

  delay(10);
}
