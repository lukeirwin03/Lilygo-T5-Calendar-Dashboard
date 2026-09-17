#include <Arduino.h>
#include <sys/time.h>
#include "config.h"
#include "display_manager.h"
#include "conn_screen.h"
#include "diff_test.h"
#include "ui.h"
#include "ui_settings.h"
#include "refresh_hygiene.h"
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

// Simulated clock: starts at 9:42 AM on the real boot date and then runs
// in real time, giving the demo a valid "today" so the sliding timeline
// window, now marker, and scroll arrows are all live. Long-press the
// button to jump +2h (wrapping past midnight back to 6 AM) and watch the
// window slide/freeze across the day.
static constexpr int DEMO_START_HOUR = 9;
static constexpr int DEMO_START_MIN  = 42;

static void initSimClock() {
  setenv("TZ", config::TIMEZONE, 1);
  tzset();
  time_t boot = time(nullptr);
  struct tm simTm;
  localtime_r(&boot, &simTm);
  simTm.tm_hour = DEMO_START_HOUR;
  simTm.tm_min  = DEMO_START_MIN;
  simTm.tm_sec  = 0;
  timeval tv;
  tv.tv_sec = mktime(&simTm);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

static void logSimClock() {
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  Serial.printf("[demo] Simulated clock: %04d-%02d-%02d %02d:%02d:%02d "
                "(long-press button to jump +2h)\n",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                lt.tm_hour, lt.tm_min, lt.tm_sec);
}

// Jump the simulated clock forward by `hours`, wrapping within the same
// demo day (past midnight → back to 6 AM) so the boot-day events stay
// valid. Re-feeds the events so a fresh render runs (also resets any
// manual timeline scroll).
static void demoJumpTime(int hours) {
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  int newHour = lt.tm_hour + hours;
  if (newHour >= 24) newHour -= 18;   // wrap to morning of the same day
  lt.tm_hour = newHour;
  lt.tm_min = 0;
  lt.tm_sec = 0;
  timeval tv;
  tv.tv_sec = mktime(&lt);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  logSimClock();
  ui::setEvents(testEvents, TEST_EVENT_COUNT);   // pending render + scroll reset
}

// Delay that returns true as soon as any touch or button press lands —
// callers use it to make the connection animation skippable.
static bool demoDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (touch_input::isTouched() || digitalRead(config::BUTTON_PIN) == LOW) return true;
    delay(20);
  }
  return false;
}

// Play the cold-boot connection screen so it can be photographed and the
// flash-free differential refresh tuned on the demo build (dashboard env
// only shows it on a true no-cache cold boot). Plays two full stage
// cycles — the first all-success, the second ending in a failure label —
// then continues; any touch or button press skips ahead.
static void demoConnectionAnimation() {
  Serial.println("[demo] Connection screen (2 cycles, touch/button skips)...");
  conn_screen::begin();
  for (int s = 0; s < 4; s++) {
    conn_screen::stageStart(s);
    if (demoDelay(800)) return;
    conn_screen::tick();
    if (demoDelay(800)) return;
    conn_screen::stageDone(s, true);
    if (demoDelay(500)) return;
  }
  // Second cycle: same stages, but the final one fails — exercises the
  // failure-label erase path ("No payload received").
  for (int s = 0; s < 4; s++) {
    conn_screen::stageStart(s);
    if (demoDelay(800)) return;
    conn_screen::stageDone(s, s == 3 ? false : true);
    if (demoDelay(500)) return;
  }
  demoDelay(1200);   // hold the final state for photos
}

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
      // INT is LOW (touched=true) but GT911 returned no point data —
      // poll() logs this edge-triggered (once per rest, not every poll).
      touched = false;
    }
  }

  ui::updateTouch(touched, x, y);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  setCpuFrequencyMhz(160);  // save ~30% active power vs default 240 MHz
  initSimClock();
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

  refresh_hygiene::begin();

  // Calibration (opt-in via config::DIFF_TEST_AT_BOOT): the
  // differential-refresh test screen loops until the button is pressed,
  // then the connection animation plays.
  if (config::DIFF_TEST_AT_BOOT) diff_test::run();

  // Photogenic boot: play the connection screen animation (skippable).
  demoConnectionAnimation();

  sd_storage::begin();
  settings::init();

  initTestEvents();
  ui::setEvents(testEvents, TEST_EVENT_COUNT);

  lastActivityMs = millis();
}

// Full cleared flash of a freshly rendered view — the mode-0 sequence.
// Also the refresh-hygiene reset: pair every call with
// refresh_hygiene::markFullFlash().
static void fullFlashRender() {
  display_mgr::powerOn();
  epd_clear();
  ui::render();
  display_mgr::fullRefresh();
  display_mgr::powerOff();
}

static void doRender() {
  // Partial refresh modes 0..5:
  //   0 full, 1 settings modal diff, 2 daily detail, 3 focus scroll,
  //   4 settings close reflash, 5 gray view change.
  // When GRAY_DIFF_ENABLED, modes 2/3/5 first check refresh_hygiene:
  // if the flash-free credit cap or a time backstop was hit, the
  // transition silently upgrades to a full cleared flash (paying down
  // the accumulated diff debt) instead of another differential update.
  // Modes 3 and 5 additionally ride cadences checked before the
  // hygiene ledger: every SCROLL_FLASH_EVERY-th scroll reflashs the
  // focus rows; every NAV_FLASH_EVERY-th day nav full-flashes.
  int mode = ui::refreshMode();
  if (mode == 2) {  // REFRESH_PARTIAL_DAILY (detail open/back)
    if (config::GRAY_DIFF_ENABLED && refresh_hygiene::wantsFlash()) {
      fullFlashRender();
      refresh_hygiene::markFullFlash();
      Serial.println("[demo] Hygiene piggyback — full flash (daily detail)");
    } else if (config::GRAY_DIFF_ENABLED) {
      ui::render();
      if (ui::pushDailyDetailGray()) {
        refresh_hygiene::creditDiff();
        Serial.println("[demo] Gray diff (daily detail)");
      } else {
        // Gray push failed (record alloc) — legacy cleared partial.
        int dx, dy, dw, dh;
        ui::getDailyDirtyRect(dx, dy, dw, dh);
        display_mgr::partialRefresh(dx, dy, dw, dh);
        Serial.println("[demo] Partial refresh (daily, fallback)");
      }
    } else {
      ui::render();
      int dx, dy, dw, dh;
      ui::getDailyDirtyRect(dx, dy, dw, dh);
      display_mgr::partialRefresh(dx, dy, dw, dh);
      Serial.println("[demo] Partial refresh (daily)");
    }
  } else if (mode == 1) {  // REFRESH_PARTIAL_SETTINGS (modal)
    ui::render();
    if (ui_settings::diffPush()) {
      refresh_hygiene::creditDiff();
    } else {
      int sx, sy, sw, sh;
      ui::getSettingsDirtyRect(sx, sy, sw, sh);
      display_mgr::partialRefresh(sx, sy, sw, sh);
    }
    Serial.println("[demo] Diff update (settings)");
  } else if (mode == 3) {  // REFRESH_PARTIAL_FOCUS (timeline scroll)
    if (config::GRAY_DIFF_ENABLED && ui::focusScrollFlashDue()) {
      // Scroll cadence: every Nth scroll gets a cleared reflash of the
      // focus rows (row-range semantics — full-width clear of those rows)
      // instead of another gray diff. Scoped like the settings modal's
      // close reflash; does NOT reset the global hygiene ledger.
      ui::render();
      int fx, fy, fw, fh;
      ui::getFocusGhostRect(fx, fy, fw, fh);
      display_mgr::partialRefresh(fx, fy, fw, fh);
      Serial.println("[demo] Scroll cadence reflash (focus)");
    } else if (config::GRAY_DIFF_ENABLED && refresh_hygiene::wantsFlash()) {
      fullFlashRender();
      refresh_hygiene::markFullFlash();
      Serial.println("[demo] Hygiene piggyback — full flash (focus scroll)");
    } else if (config::GRAY_DIFF_ENABLED) {
      ui::render();
      if (ui::pushFocusScrollGray()) {
        refresh_hygiene::creditDiff();
        Serial.println("[demo] Gray diff (focus scroll)");
      } else {
        // Gray push failed (record alloc) — legacy ghost push.
        int fx, fy, fw, fh;
        ui::getFocusGhostRect(fx, fy, fw, fh);
        display_mgr::ghostRefresh(fx, fy, fw, fh);
        refresh_hygiene::creditGhost();  // ghost pushes dirty the panel too
        Serial.println("[demo] Ghost refresh (focus timeline, fallback)");
      }
    } else {
      ui::render();
      int fx, fy, fw, fh;
      ui::getFocusGhostRect(fx, fy, fw, fh);
      display_mgr::ghostRefresh(fx, fy, fw, fh);
      refresh_hygiene::creditGhost();    // ghost pushes dirty the panel too
      Serial.println("[demo] Ghost refresh (focus timeline)");
    }
  } else if (mode == 4) {  // REFRESH_SETTINGS_CLOSE (modal closing, band-local cleared reflash — no hygiene reset or credit)
    ui::render();
    ui_settings::closeReflash();
    Serial.println("[demo] Reflash (settings close)");
  } else if (mode == 5) {  // REFRESH_GRAY (day nav; ui only sets this when GRAY_DIFF_ENABLED)
    if (ui::navFlashDue()) {
      // Nav cadence: every NAV_FLASH_EVERY-th day nav full-flashes — day
      // navs are the heaviest gray diff in the app (~whole-screen erase
      // per tap) and ghost fast. Resets the global hygiene ledger too.
      fullFlashRender();
      refresh_hygiene::markFullFlash();
      Serial.println("[demo] Nav cadence — full flash (day nav)");
    } else if (refresh_hygiene::wantsFlash()) {
      fullFlashRender();
      refresh_hygiene::markFullFlash();
      Serial.println("[demo] Hygiene piggyback — full flash (day nav)");
    } else {
      ui::render();
      if (ui::pushViewGray()) {
        refresh_hygiene::creditDiff();
        Serial.println("[demo] Gray diff (day nav)");
      } else {
        // Gray push failed (record alloc) — full flash fallback. The
        // view is already rendered; just clear and push it.
        display_mgr::powerOn();
        epd_clear();
        display_mgr::fullRefresh();
        display_mgr::powerOff();
        refresh_hygiene::markFullFlash();
        Serial.println("[demo] Full refresh (day nav, fallback)");
      }
    }
  } else {
    fullFlashRender();
    refresh_hygiene::markFullFlash();
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

// Button: short press toggles the settings modal; long press (≥800 ms)
// jumps the simulated clock +2h so the sliding-window states (morning
// clamp, mid-day slide, freeze, evening ▼ clamp) can be photographed.
static bool s_btnDown = false;
static unsigned long s_btnDownMs = 0;
static bool s_btnLongFired = false;
static constexpr unsigned long BTN_LONG_PRESS_MS = 800;

static void handleDemoButton(unsigned long now) {
  bool pressed = digitalRead(config::BUTTON_PIN) == LOW;

  if (pressed && !s_btnDown) {
    s_btnDown = true;
    s_btnDownMs = now;
    s_btnLongFired = false;
  }
  if (pressed && s_btnDown && !s_btnLongFired
      && now - s_btnDownMs >= BTN_LONG_PRESS_MS) {
    s_btnLongFired = true;
    lastActivityMs = now;
    demoJumpTime(2);
  }
  if (!pressed && s_btnDown) {
    s_btnDown = false;
    if (!s_btnLongFired && now - s_btnDownMs < BTN_LONG_PRESS_MS
        && now - lastButtonMs > 300) {
      lastButtonMs = now;
      lastActivityMs = now;
      ui::toggleSettings();
      Serial.println("[demo] Button -> toggle settings");
    }
  }
}

void loop() {
  unsigned long now = millis();

  handleTouch();
  handleDemoButton(now);

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
