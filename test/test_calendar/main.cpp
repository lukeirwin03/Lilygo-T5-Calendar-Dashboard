#include <unity.h>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Shade hashing — djb2 variant used by CalendarDashboard::shadeForCalendar()
// Maps calendar names to one of 4 spaced-apart grayscale shades.
// ---------------------------------------------------------------------------
static uint8_t shadeForCalendar(const char* name) {
  if (!name || !name[0]) return 7;
  unsigned long h = 5381;
  for (const char* p = name; *p; p++) h = ((h << 5) + h) + *p;
  const uint8_t shades[] = {3, 7, 10, 13};
  return shades[h % 4];
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
  TEST_ASSERT_TRUE(s1 == 3 || s1 == 7 || s1 == 10 || s1 == 13);
  TEST_ASSERT_TRUE(s2 == 3 || s2 == 7 || s2 == 10 || s2 == 13);
}

void test_shade_always_in_valid_range(void) {
  const char* names[] = {"a", "b", "ab", "abc", "test", "work", "home", "main", "personal", "family"};
  for (const char* n : names) {
    uint8_t s = shadeForCalendar(n);
    TEST_ASSERT_TRUE(s >= 0 && s <= 15);
    TEST_ASSERT_TRUE(s == 3 || s == 7 || s == 10 || s == 13);
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

int main() {
  UNITY_BEGIN();

  // Shade hashing
  RUN_TEST(test_shade_empty_name_returns_default);
  RUN_TEST(test_shade_consistent_for_same_name);
  RUN_TEST(test_shade_different_names_may_differ);
  RUN_TEST(test_shade_always_in_valid_range);

  // Date parsing
  RUN_TEST(test_parse_date_only);
  RUN_TEST(test_parse_datetime);
  RUN_TEST(test_parse_datetime_with_timezone);
  RUN_TEST(test_parse_null_or_empty);
  RUN_TEST(test_parse_invalid_format);

  // Date formatting
  RUN_TEST(test_date_to_string);
  RUN_TEST(test_date_to_string_single_digit);

  UNITY_END();
  return 0;
}
