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

  UNITY_END();
  return 0;
}
