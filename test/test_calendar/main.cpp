#include <unity.h>
#include <cstring>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// Shade hashing — djb2 variant used by CalendarDashboard::shadeForCalendar()
// "main" is pinned to the light shade 13; other calendar names hash across
// the three darker shades {3, 7, 10}.
// ---------------------------------------------------------------------------
static uint8_t shadeForCalendar(const char* name) {
  if (!name || !name[0]) return 7;
  if (strcmp(name, "main") == 0) return 13;
  uint32_t h = 5381;
  for (const char* p = name; *p; p++) h = ((h << 5) + h) + *p;
  const uint8_t shades[] = {3, 7, 10};
  return shades[h % 3];
}

void test_shade_empty_name_returns_default(void) {
  TEST_ASSERT_EQUAL(7, shadeForCalendar(""));
  TEST_ASSERT_EQUAL(7, shadeForCalendar(nullptr));
}

void test_shade_consistent_for_same_name(void) {
  uint8_t s1 = shadeForCalendar("main");
  uint8_t s2 = shadeForCalendar("main");
  TEST_ASSERT_EQUAL(s1, s2);
}

void test_shade_different_names_may_differ(void) {
  uint8_t s1 = shadeForCalendar("work");
  uint8_t s2 = shadeForCalendar("personal");
  // They might be the same by chance, but at least verify they're in the valid set
  TEST_ASSERT_TRUE(s1 == 3 || s1 == 7 || s1 == 10);
  TEST_ASSERT_TRUE(s2 == 3 || s2 == 7 || s2 == 10);
}

void test_shade_main_is_light(void) {
  TEST_ASSERT_EQUAL(13, shadeForCalendar("main"));
}

void test_shade_non_main_avoids_main_shade(void) {
  const char* names[] = {"a", "b", "ab", "abc", "test", "work", "home", "personal", "family"};
  for (const char* n : names) {
    uint8_t s = shadeForCalendar(n);
    TEST_ASSERT_TRUE(s == 3 || s == 7 || s == 10);
  }
}

// ---------------------------------------------------------------------------
// ISO date/time parsing — used by CalendarDashboard::parseIsoDateTime()
// ---------------------------------------------------------------------------
static void parseIsoDateTime(const char* iso, int& year, int& month, int& day,
                             int& hour, int& min) {
  year = month = day = 0;
  hour = min = 0;
  if (!iso || !iso[0]) return;
  if (sscanf(iso, "%d-%d-%d", &year, &month, &day) < 3) return;
  const char* t = strchr(iso, 'T');
  if (t) sscanf(t + 1, "%d:%d", &hour, &min);
}

void test_parse_date_only(void) {
  int y, m, d, h, min;
  parseIsoDateTime("2026-07-05", y, m, d, h, min);
  TEST_ASSERT_EQUAL(2026, y);
  TEST_ASSERT_EQUAL(7, m);
  TEST_ASSERT_EQUAL(5, d);
  TEST_ASSERT_EQUAL(0, h);
  TEST_ASSERT_EQUAL(0, min);
}

void test_parse_datetime(void) {
  int y, m, d, h, min;
  parseIsoDateTime("2026-07-05T15:30", y, m, d, h, min);
  TEST_ASSERT_EQUAL(2026, y);
  TEST_ASSERT_EQUAL(7, m);
  TEST_ASSERT_EQUAL(5, d);
  TEST_ASSERT_EQUAL(15, h);
  TEST_ASSERT_EQUAL(30, min);
}

void test_parse_datetime_with_timezone(void) {
  int y, m, d, h, min;
  parseIsoDateTime("2026-07-05T15:30-05:00", y, m, d, h, min);
  TEST_ASSERT_EQUAL(2026, y);
  TEST_ASSERT_EQUAL(15, h);
  TEST_ASSERT_EQUAL(30, min);
}

void test_parse_null_or_empty(void) {
  int y, m, d, h, min;
  parseIsoDateTime(nullptr, y, m, d, h, min);
  TEST_ASSERT_EQUAL(0, y);
  TEST_ASSERT_EQUAL(0, m);
  TEST_ASSERT_EQUAL(0, d);

  parseIsoDateTime("", y, m, d, h, min);
  TEST_ASSERT_EQUAL(0, y);
}

void test_parse_invalid_format(void) {
  int y, m, d, h, min;
  parseIsoDateTime("not-a-date", y, m, d, h, min);
  TEST_ASSERT_EQUAL(0, y);
}

// ---------------------------------------------------------------------------
// Date-to-string formatting
// ---------------------------------------------------------------------------
static void dateToString(int year, int month, int day, char* out, size_t len) {
  snprintf(out, len, "%04d-%02d-%02d", year, month, day);
}

// Mirror of CalendarDashboard::handlePayload's event-window filter.
// Window is [todayStart - N*86400, todayStart + (N+1)*86400) — i.e. today,
// the N days before, and the N days after (half-open so +N is inclusive).
static bool isInWindow(int todayY, int todayMo, int todayD, int contextDays,
                       int evY, int evMo, int evD) {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = todayY - 1900; t.tm_mon = todayMo - 1; t.tm_mday = todayD;
  t.tm_isdst = -1;
  time_t todayStart = mktime(&t);
  time_t windowStart = todayStart - (time_t)contextDays * 86400;
  time_t windowEnd   = todayStart + (time_t)(contextDays + 1) * 86400;

  struct tm e;
  memset(&e, 0, sizeof(e));
  e.tm_year = evY - 1900; e.tm_mon = evMo - 1; e.tm_mday = evD;
  e.tm_isdst = -1;
  time_t evDate = mktime(&e);

  return evDate >= windowStart && evDate < windowEnd;
}

// Mirror of CalendarDashboard::handlePayload's past-day date computation:
// today + offsetDays, normalized across month/year boundaries by mktime.
// Used to read /cal/cache for past days in the bidirectional window.
static void dateOffset(int y, int mo, int d, int offsetDays, char* out, size_t len) {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon  = mo - 1;
  t.tm_mday = d;
  t.tm_hour = 0;
  t.tm_min  = 0;
  t.tm_sec  = 0;
  t.tm_isdst = -1;
  t.tm_mday += offsetDays;
  mktime(&t);  // normalizes across month/year boundaries
  strftime(out, len, "%Y-%m-%d", &t);
}

void test_date_to_string(void) {
  char buf[12];
  dateToString(2026, 7, 5, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2026-07-05", buf);
}

void test_date_to_string_single_digit(void) {
  char buf[12];
  dateToString(2026, 1, 1, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2026-01-01", buf);
}

// ---- Bidirectional event window (mirrors handlePayload) ----
void test_window_today_always_included(void) {
  // contextDays=7, today=2026-08-11
  TEST_ASSERT_TRUE(isInWindow(2026, 8, 11, 7, 2026, 8, 11));
}

void test_window_past_edge_inclusive(void) {
  // 7 days prior is the inclusive past edge
  TEST_ASSERT_TRUE(isInWindow(2026, 8, 11, 7, 2026, 8, 4));
}

void test_window_past_just_outside(void) {
  // 8 days prior is excluded
  TEST_ASSERT_FALSE(isInWindow(2026, 8, 11, 7, 2026, 8, 3));
}

void test_window_future_edge_inclusive(void) {
  // 7 days after is the inclusive future edge
  TEST_ASSERT_TRUE(isInWindow(2026, 8, 11, 7, 2026, 8, 18));
}

void test_window_future_just_outside(void) {
  // 8 days after is excluded
  TEST_ASSERT_FALSE(isInWindow(2026, 8, 11, 7, 2026, 8, 19));
}

void test_window_n1_symmetric(void) {
  // contextDays=1: yesterday, today, tomorrow included; day-before-yesterday and day-after-tomorrow excluded
  TEST_ASSERT_TRUE(isInWindow(2026, 8, 11, 1, 2026, 8, 10));   // -1
  TEST_ASSERT_TRUE(isInWindow(2026, 8, 11, 1, 2026, 8, 11));   //  0
  TEST_ASSERT_TRUE(isInWindow(2026, 8, 11, 1, 2026, 8, 12));   // +1
  TEST_ASSERT_FALSE(isInWindow(2026, 8, 11, 1, 2026, 8, 9));   // -2
  TEST_ASSERT_FALSE(isInWindow(2026, 8, 11, 1, 2026, 8, 13));  // +2
}

// ---- Past-day date computation (mirrors handlePayload cache-load loop) ----
void test_date_offset_simple_week(void) {
  char buf[11];
  dateOffset(2026, 8, 11, -7, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2026-08-04", buf);
}

void test_date_offset_month_boundary_nonleap(void) {
  char buf[11];
  dateOffset(2026, 3, 1, -1, buf, sizeof(buf));   // day before Mar 1, 2026
  TEST_ASSERT_EQUAL_STRING("2026-02-28", buf);    // 2026 is NOT a leap year
}

void test_date_offset_month_boundary_leap(void) {
  char buf[11];
  dateOffset(2024, 3, 1, -1, buf, sizeof(buf));   // day before Mar 1, 2024
  TEST_ASSERT_EQUAL_STRING("2024-02-29", buf);    // 2024 IS a leap year
}

void test_date_offset_year_boundary(void) {
  char buf[11];
  dateOffset(2026, 1, 1, -1, buf, sizeof(buf));   // day before Jan 1
  TEST_ASSERT_EQUAL_STRING("2025-12-31", buf);
}

void test_date_offset_zero_is_same_day(void) {
  char buf[11];
  dateOffset(2026, 8, 11, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2026-08-11", buf);
}

// ---------------------------------------------------------------------------
// Cache-merge dedup — mirrors CalendarDashboard::handlePayload's SD day-cache
// augmentation. The publisher sends past events too (docs recommend
// now−9d…now+9d), so cached past days must be deduped against events already
// contributed by the live payload (and against each other).
// ---------------------------------------------------------------------------
// Mirror of the firmware's CalendarEvent, limited to the fields the dedup
// logic touches. (The real header pulls in ArduinoJson via dashboard.h and
// can't be compiled in the native test env.)
struct CalendarEvent {
  char title[64];
  char calendar[24];   // source calendar name (matches firmware field width)
  char date[11];       // YYYY-MM-DD
  int  startHour;
  int  startMin;
  int  durationMin;
  bool allDay;
};

// Mirror of the file-scope sameOccurrence() helper in calendar_dashboard.cpp.
// Two events are the same occurrence if date, title, start time, all-day
// flag and calendar name match.
static bool sameOccurrence(const CalendarEvent& a, const CalendarEvent& b) {
  return a.allDay == b.allDay
      && a.startHour == b.startHour
      && a.startMin == b.startMin
      && strcmp(a.date, b.date) == 0
      && strcmp(a.title, b.title) == 0
      && strcmp(a.calendar, b.calendar) == 0;
}

// Mirror of handlePayload's per-day merge: payload events live at
// events[0..payloadCount-1]; append cached[] events that don't duplicate an
// event already merged (payload events plus cached events kept so far).
// Returns the new total count, capped at maxEvents.
static int mergeCachedEvents(CalendarEvent* events, int payloadCount,
                             const CalendarEvent* cached, int cachedCount,
                             int maxEvents) {
  int kept = 0;
  int room = maxEvents - payloadCount;
  for (int i = 0; i < cachedCount && kept < room; i++) {
    events[payloadCount + kept] = cached[i];  // stage candidate at the tail
    bool dupe = false;
    for (int j = 0; j < payloadCount + kept; j++) {
      if (sameOccurrence(events[payloadCount + kept], events[j])) { dupe = true; break; }
    }
    if (!dupe) kept++;
  }
  return payloadCount + kept;
}

static CalendarEvent makeEvent(const char* title, const char* date,
                               int hour, int min, bool allDay,
                               int durationMin = 60,
                               const char* calendar = "main") {
  CalendarEvent e = {};
  snprintf(e.title, sizeof(e.title), "%s", title);
  snprintf(e.calendar, sizeof(e.calendar), "%s", calendar);
  snprintf(e.date, sizeof(e.date), "%s", date);
  e.startHour = hour;
  e.startMin = min;
  e.durationMin = durationMin;
  e.allDay = allDay;
  return e;
}

void test_merge_identical_cached_event_skipped(void) {
  CalendarEvent events[4];
  events[0] = makeEvent("Standup", "2026-08-20", 9, 0, false, 30);
  CalendarEvent cached[1] = { makeEvent("Standup", "2026-08-20", 9, 0, false, 30) };
  int total = mergeCachedEvents(events, 1, cached, 1, 4);
  TEST_ASSERT_EQUAL(1, total);  // duplicate dropped, count stays at payloadCount
  TEST_ASSERT_EQUAL_STRING("Standup", events[0].title);
}

void test_merge_same_title_time_different_date_kept(void) {
  CalendarEvent events[4];
  events[0] = makeEvent("Standup", "2026-08-20", 9, 0, false);
  CalendarEvent cached[1] = { makeEvent("Standup", "2026-08-19", 9, 0, false) };
  int total = mergeCachedEvents(events, 1, cached, 1, 4);
  TEST_ASSERT_EQUAL(2, total);
  TEST_ASSERT_EQUAL_STRING("2026-08-19", events[1].date);
}

void test_merge_same_date_title_different_time_kept(void) {
  CalendarEvent events[4];
  events[0] = makeEvent("Standup", "2026-08-20", 9, 0, false);
  CalendarEvent cached[1] = { makeEvent("Standup", "2026-08-20", 10, 30, false) };
  int total = mergeCachedEvents(events, 1, cached, 1, 4);
  TEST_ASSERT_EQUAL(2, total);
  TEST_ASSERT_EQUAL(10, events[1].startHour);
  TEST_ASSERT_EQUAL(30, events[1].startMin);
}

void test_merge_duplicate_within_cache_collapsed(void) {
  CalendarEvent events[4];
  events[0] = makeEvent("Payload event", "2026-08-20", 8, 0, false);
  CalendarEvent cached[2] = {
    makeEvent("Cached event", "2026-08-19", 14, 0, false),
    makeEvent("Cached event", "2026-08-19", 14, 0, false),  // same occurrence again
  };
  int total = mergeCachedEvents(events, 1, cached, 2, 4);
  TEST_ASSERT_EQUAL(2, total);  // collapsed to a single copy
  TEST_ASSERT_EQUAL_STRING("Cached event", events[1].title);
}

void test_merge_respects_max_events(void) {
  CalendarEvent events[4];
  events[0] = makeEvent("Payload A", "2026-08-20", 8, 0, false);
  events[1] = makeEvent("Payload B", "2026-08-21", 9, 0, false);
  CalendarEvent cached[3] = {
    makeEvent("Cached A", "2026-08-19", 10, 0, false),
    makeEvent("Cached B", "2026-08-18", 11, 0, false),
    makeEvent("Cached C", "2026-08-17", 12, 0, false),
  };
  int total = mergeCachedEvents(events, 2, cached, 3, 4);
  TEST_ASSERT_EQUAL(4, total);  // 2 payload + only 2 of 3 cached fit
  TEST_ASSERT_EQUAL_STRING("Cached A", events[2].title);
  TEST_ASSERT_EQUAL_STRING("Cached B", events[3].title);
}

void test_merge_same_occurrence_different_calendar_kept(void) {
  // All-day "Holiday" from two different calendars is NOT a duplicate —
  // the calendar name is part of the identity.
  CalendarEvent events[4];
  events[0] = makeEvent("Holiday", "2026-08-20", 0, 0, true, 0, "personal");
  CalendarEvent cached[1] = { makeEvent("Holiday", "2026-08-20", 0, 0, true, 0, "work") };
  int total = mergeCachedEvents(events, 1, cached, 1, 4);
  TEST_ASSERT_EQUAL(2, total);  // different calendar -> kept, count increments
  TEST_ASSERT_EQUAL_STRING("work", events[1].calendar);
}

void test_merge_payload_copy_wins(void) {
  CalendarEvent events[4];
  events[0] = makeEvent("Standup", "2026-08-20", 9, 0, false, 30);  // payload copy
  CalendarEvent cached[1] = { makeEvent("Standup", "2026-08-20", 9, 0, false, 999) };  // stale cached copy
  int total = mergeCachedEvents(events, 1, cached, 1, 4);
  TEST_ASSERT_EQUAL(1, total);
  TEST_ASSERT_EQUAL(30, events[0].durationMin);  // payload's copy survived
}

// ---------------------------------------------------------------------------
// Weekly-view focus-column block geometry — mirrors computeBlockHeight() and
// the blockY computation in src/ui.cpp. Short events take the max of the
// 28-50 px text ramp and the proportional time slot so back-to-back events
// fill their slots on dense timelines (only the deliberate EVENT_GAP remains);
// long events take the max of minH and the LONG_EVENT_CAP_MIN-capped
// proportional height.
// ---------------------------------------------------------------------------
static const int EVENT_GAP            = 4;    // px gap rendered between blocks
static const int SHORT_EVENT_THRESHOLD = 60;  // min; events <= this use 1-line format
static const int SHORT_EVENT_MIN_H     = 28;  // px; 1-line block min height
static const int SHORT_EVENT_MAX_H     = 50;  // px; 1-line block max height (at 60 min)
static const int LONG_EVENT_CAP_MIN    = 480; // min; longer events plateau

static int imax(int a, int b) { return (a > b) ? a : b; }
static int imin(int a, int b) { return (a < b) ? a : b; }

static int computeBlockHeight(int durationMin, int timelineH, int dayMinutes, int minH) {
  int proportional = (durationMin * timelineH) / dayMinutes;
  if (durationMin <= SHORT_EVENT_THRESHOLD) {
    int clamped = imax(30, durationMin);
    int ramp = SHORT_EVENT_MIN_H
               + (clamped - 30) * (SHORT_EVENT_MAX_H - SHORT_EVENT_MIN_H) / 30;
    return imax(ramp, proportional);
  }
  int cappedDur = imin(durationMin, LONG_EVENT_CAP_MIN);
  int cappedProportional = (cappedDur * timelineH) / dayMinutes;
  return imax(minH, cappedProportional);
}

// Mirror of the weekly-view focus-column positioning:
// top + ((visStart - rangeStart) * timelineH) / rangeMinutes
static int blockY(int top, int visStart, int rangeStart, int timelineH, int rangeMinutes) {
  return top + ((visStart - rangeStart) * timelineH) / rangeMinutes;
}

void test_blockheight_dense_backtoback_fills_slot(void) {
  // The bug scenario: 6 h focus range, three back-to-back 60-min events
  // (1-2, 2-3, 3-4 PM). Each block must fill its ~56 px proportional slot.
  const int timelineH = 338, dayMinutes = 360;
  const int rangeStart = 720;                 // noon
  const int starts[3] = {780, 840, 900};
  const int slotH = (60 * timelineH) / dayMinutes;  // 56
  int y[3], h[3];
  for (int i = 0; i < 3; i++) {
    h[i] = computeBlockHeight(60, timelineH, dayMinutes, 40);
    TEST_ASSERT_EQUAL(slotH, h[i]);
    y[i] = blockY(0, starts[i], rangeStart, timelineH, dayMinutes);
  }
  for (int i = 0; i + 1 < 3; i++) {
    // Consecutive blocks leave only the deliberate EVENT_GAP of white space
    // (plus at most 1 px of integer-truncation drift in the independent
    // blockY computations — floor(a+b) >= floor(a)+floor(b) guarantees the
    // gap never falls below EVENT_GAP, so blocks never visually overlap).
    int gap = y[i + 1] - (y[i] + h[i] - EVENT_GAP);
    TEST_ASSERT_TRUE(gap == EVENT_GAP || gap == EVENT_GAP + 1);
  }
}

void test_blockheight_sparse_keeps_text_floor(void) {
  // 18 h focus range: proportional slots (~18 px) fall below the text ramp,
  // so the 28-50 px readability floor applies.
  const int timelineH = 338, dayMinutes = 1080;
  TEST_ASSERT_EQUAL(50, computeBlockHeight(60, timelineH, dayMinutes, 40));  // ramp, not ~18
  TEST_ASSERT_EQUAL(28, computeBlockHeight(30, timelineH, dayMinutes, 40));  // ramp floor
}

void test_blockheight_long_uses_min_and_proportional(void) {
  TEST_ASSERT_EQUAL(84, computeBlockHeight(90, 338, 360, 63));    // (90*338)/360 beats minH
  TEST_ASSERT_EQUAL(40, computeBlockHeight(90, 338, 1080, 40));   // proportional 28 < minH 40
}

void test_blockheight_long_cap_plateau(void) {
  // 600 min capped at LONG_EVENT_CAP_MIN (480): (480*338)/1440 = 112, not 140.
  TEST_ASSERT_EQUAL(112, computeBlockHeight(600, 338, 1440, 40));
}

int main() {
  UNITY_BEGIN();

  // Shade hashing
  RUN_TEST(test_shade_empty_name_returns_default);
  RUN_TEST(test_shade_consistent_for_same_name);
  RUN_TEST(test_shade_different_names_may_differ);
  RUN_TEST(test_shade_main_is_light);
  RUN_TEST(test_shade_non_main_avoids_main_shade);

  // Date parsing
  RUN_TEST(test_parse_date_only);
  RUN_TEST(test_parse_datetime);
  RUN_TEST(test_parse_datetime_with_timezone);
  RUN_TEST(test_parse_null_or_empty);
  RUN_TEST(test_parse_invalid_format);

  // Date formatting
  RUN_TEST(test_date_to_string);
  RUN_TEST(test_date_to_string_single_digit);

  // Bidirectional event window
  RUN_TEST(test_window_today_always_included);
  RUN_TEST(test_window_past_edge_inclusive);
  RUN_TEST(test_window_past_just_outside);
  RUN_TEST(test_window_future_edge_inclusive);
  RUN_TEST(test_window_future_just_outside);
  RUN_TEST(test_window_n1_symmetric);

  // Past-day date math (cache load)
  RUN_TEST(test_date_offset_simple_week);
  RUN_TEST(test_date_offset_month_boundary_nonleap);
  RUN_TEST(test_date_offset_month_boundary_leap);
  RUN_TEST(test_date_offset_year_boundary);
  RUN_TEST(test_date_offset_zero_is_same_day);

  // Cache-merge dedup (day-cache augmentation)
  RUN_TEST(test_merge_identical_cached_event_skipped);
  RUN_TEST(test_merge_same_title_time_different_date_kept);
  RUN_TEST(test_merge_same_date_title_different_time_kept);
  RUN_TEST(test_merge_duplicate_within_cache_collapsed);
  RUN_TEST(test_merge_respects_max_events);
  RUN_TEST(test_merge_same_occurrence_different_calendar_kept);
  RUN_TEST(test_merge_payload_copy_wins);

  // Weekly-view block geometry (dense/sparse timelines, long-event caps)
  RUN_TEST(test_blockheight_dense_backtoback_fills_slot);
  RUN_TEST(test_blockheight_sparse_keeps_text_floor);
  RUN_TEST(test_blockheight_long_uses_min_and_proportional);
  RUN_TEST(test_blockheight_long_cap_plateau);

  UNITY_END();
  return 0;
}
