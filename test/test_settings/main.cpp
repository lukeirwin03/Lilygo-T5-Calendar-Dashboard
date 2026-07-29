#include <unity.h>
#include <cstdint>

// Value arrays (duplicated from ui_settings.cpp)
static const int startValues[] = {5, 6, 7, 8, 9};
static const int endValues[] = {20, 21, 22, 23};
static const uint32_t refreshValues[] = {1800, 3600, 7200, 14400, 21600};
static const uint32_t inactivityValues[] = {15, 30, 45, 60, 120, 300};
static const uint32_t retentionValues[] = {30, 90, 180, 365};

// Cycle helper (same logic as ui_settings::cycleSetting)
template<typename T>
static T cycleValue(const T* arr, int count, T current, int delta) {
  int idx = 0;
  for (int i = 0; i < count; i++) {
    if (arr[i] == current) { idx = i; break; }
  }
  int newIdx = ((idx + delta) % count + count) % count;
  return arr[newIdx];
}

void test_start_hour_cycle(void) {
  TEST_ASSERT_EQUAL(6, cycleValue(startValues, 5, 5, +1));
  TEST_ASSERT_EQUAL(5, cycleValue(startValues, 5, 9, +1));  // wraps
  TEST_ASSERT_EQUAL(9, cycleValue(startValues, 5, 5, -1));  // wraps back
}

void test_refresh_values(void) {
  TEST_ASSERT_EQUAL(3600, cycleValue(refreshValues, 5, (uint32_t)1800, +1));
  TEST_ASSERT_EQUAL(1800, cycleValue(refreshValues, 5, (uint32_t)21600, +1));  // wraps
}

void test_inactivity_values(void) {
  TEST_ASSERT_EQUAL(30, cycleValue(inactivityValues, 6, (uint32_t)15, +1));
  TEST_ASSERT_EQUAL(15, cycleValue(inactivityValues, 6, (uint32_t)300, +1));  // wraps
  TEST_ASSERT_EQUAL(300, cycleValue(inactivityValues, 6, (uint32_t)15, -1));  // wraps back
}

void test_retention_values(void) {
  TEST_ASSERT_EQUAL(90, cycleValue(retentionValues, 4, (uint32_t)30, +1));
  TEST_ASSERT_EQUAL(30, cycleValue(retentionValues, 4, (uint32_t)365, +1));  // wraps
}

// Settings Data struct layout test
struct Data {
  uint8_t  day_start_hour;
  uint8_t  day_end_hour;
  uint32_t refresh_interval_s;
  uint32_t inactivity_timeout_s;
  uint32_t history_retention_d;
  char     wifi_ssid[33];
  char     wifi_password[65];
  char     mqtt_host[64];
  uint16_t mqtt_port;
  char     mqtt_topic[64];
};

void test_settings_struct_defaults(void) {
  Data d = {};
  d.day_start_hour = 7;
  d.day_end_hour = 22;
  d.refresh_interval_s = 7200;
  d.inactivity_timeout_s = 45;
  d.history_retention_d = 365;

  TEST_ASSERT_EQUAL(7, d.day_start_hour);
  TEST_ASSERT_EQUAL(22, d.day_end_hour);
  TEST_ASSERT_EQUAL(7200, d.refresh_interval_s);
  TEST_ASSERT_EQUAL(45, d.inactivity_timeout_s);
  TEST_ASSERT_EQUAL(365, d.history_retention_d);
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_start_hour_cycle);
  RUN_TEST(test_refresh_values);
  RUN_TEST(test_inactivity_values);
  RUN_TEST(test_retention_values);
  RUN_TEST(test_settings_struct_defaults);

  UNITY_END();
  return 0;
}
