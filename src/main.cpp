#include <Arduino.h>
#include "config.h"
#include "display_manager.h"
#include "ui.h"
#include "battery.h"
#include "touch_input.h"
#include "power_mgr.h"
#include "dashboards/calendar_dashboard.h"

static unsigned long lastButtonMs = 0;
static bool needsRender = true;

static CalendarEvent testEvents[7];
static const int TEST_EVENT_COUNT = 7;

static void initTestEvents() {
  time_t now = time(nullptr);
  struct tm today;
  localtime_r(&now, &today);

  // Test event definitions (matching the old demo data)
  struct EvDef {
    const char* title; const char* loc; const char* desc;
    int dayOff; int hr, min, dur; bool allDay; uint8_t shade;
  } defs[] = {
    { "Summer Art",      "Aksarben",          "Final project deadline", 0,  0,  0,  0, true,  5 },
    { "Chiro",           "West Omaha Chiro",  "Routine adjustment",     0, 15, 30, 30, false, 8 },
    { "MEETING",         "Conference Room",   "Q2 planning sync",       1, 15, 20, 60, false, 5 },
    { "Lunch w/ team",   "Old Market",        "Birthday celebration",   1, 12,  0, 60, false, 12 },
    { "Dentist",         "Midtown Dental",    "Cleaning and checkup",   2,  9,  0, 45, false, 8 },
    { "Code review",     "Zoom",              "Review dashboard PR",    2, 14,  0, 90, false, 5 },
    { "All-hands",       "Auditorium",        "Company updates",        3, 10,  0, 60, false, 12 },
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
    touch_input::poll(x, y);
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
  touch_input::begin();
  pinMode(config::BUTTON_PIN, INPUT_PULLUP);
  lastButtonMs = millis();

  initTestEvents();
  ui::setEvents(testEvents, TEST_EVENT_COUNT);

  needsRender = true;
}

void loop() {
  unsigned long now = millis();

  handleTouch();
  if (ui::needsRender()) {
    needsRender = true;
  }

  // Button cycles demo screens
  if (digitalRead(config::BUTTON_PIN) == LOW) {
    if (now - lastButtonMs > 300) {
      lastButtonMs = now;
      ui::next();
      needsRender = true;
      Serial.println("[demo] Button -> next screen");
    }
  }

  if (needsRender) {
    display_mgr::powerOn();
    epd_clear();
    ui::render();
    display_mgr::fullRefresh();
    display_mgr::powerOff();
    needsRender = false;
    Serial.println("[demo] Rendered screen");
  }

  delay(10);
}
