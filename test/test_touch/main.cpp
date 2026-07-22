#include <unity.h>
#include <cstdint>

// Simplified touch state machine (matches ui.cpp logic)
enum TouchPhase { TOUCH_IDLE, TOUCH_DOWN, TOUCH_LIFT_PENDING };

static TouchPhase s_phase = TOUCH_IDLE;
static unsigned long s_lastTouchSeenMs = 0;
static unsigned long s_cooldownUntilMs = 0;
static unsigned long mockMillis = 0;

static constexpr unsigned long LIFT_TIMEOUT_MS = 80;

void resetTouch() {
  s_phase = TOUCH_IDLE;
  s_lastTouchSeenMs = 0;
  s_cooldownUntilMs = 0;
  mockMillis = 0;
}

bool isInCooldown() {
  return (long)(mockMillis - s_cooldownUntilMs) < 0;
}

// Returns: 0 = no action, 1 = gesture classified
int updateTouch(bool isTouched) {
  if (isInCooldown()) {
    if (!isTouched && s_phase != TOUCH_IDLE) s_phase = TOUCH_IDLE;
    return 0;
  }

  switch (s_phase) {
    case TOUCH_IDLE:
      if (isTouched) {
        s_phase = TOUCH_DOWN;
        s_lastTouchSeenMs = mockMillis;
      }
      return 0;

    case TOUCH_DOWN:
      if (isTouched) {
        s_lastTouchSeenMs = mockMillis;
      } else {
        s_phase = TOUCH_LIFT_PENDING;
      }
      return 0;

    case TOUCH_LIFT_PENDING:
      if (isTouched) {
        s_phase = TOUCH_DOWN;
        s_lastTouchSeenMs = mockMillis;
        return 0;
      } else if (mockMillis - s_lastTouchSeenMs > LIFT_TIMEOUT_MS) {
        s_phase = TOUCH_IDLE;
        return 1;  // gesture classified
      }
      return 0;
  }
  return 0;
}

void test_idle_to_down(void) {
  resetTouch();
  int result = updateTouch(true);
  TEST_ASSERT_EQUAL(TOUCH_DOWN, s_phase);
  TEST_ASSERT_EQUAL(0, result);
}

void test_down_to_lift_pending_on_release(void) {
  resetTouch();
  updateTouch(true);          // IDLE → DOWN
  int result = updateTouch(false);  // DOWN → LIFT_PENDING
  TEST_ASSERT_EQUAL(TOUCH_LIFT_PENDING, s_phase);
  TEST_ASSERT_EQUAL(0, result);  // not classified yet
}

void test_lift_pending_returns_to_down_on_retouch(void) {
  resetTouch();
  updateTouch(true);   // IDLE → DOWN
  updateTouch(false);  // DOWN → LIFT_PENDING
  int result = updateTouch(true);  // LIFT_PENDING → DOWN
  TEST_ASSERT_EQUAL(TOUCH_DOWN, s_phase);
  TEST_ASSERT_EQUAL(0, result);  // no gesture — it was a sample gap
}

void test_lift_pending_classifies_after_timeout(void) {
  resetTouch();
  mockMillis = 100;
  updateTouch(true);   // IDLE → DOWN
  mockMillis = 110;
  updateTouch(false);  // DOWN → LIFT_PENDING
  mockMillis = 110 + LIFT_TIMEOUT_MS + 1;  // past timeout
  int result = updateTouch(false);  // classify!
  TEST_ASSERT_EQUAL(TOUCH_IDLE, s_phase);
  TEST_ASSERT_EQUAL(1, result);  // gesture classified
}

void test_sample_gap_does_not_classify(void) {
  resetTouch();
  mockMillis = 100;
  updateTouch(true);   // IDLE → DOWN at t=100
  mockMillis = 110;
  updateTouch(false);  // DOWN → LIFT_PENDING at t=110
  mockMillis = 120;    // only 10ms later — within sample gap
  updateTouch(true);   // LIFT_PENDING → DOWN (re-touched)
  TEST_ASSERT_EQUAL(TOUCH_DOWN, s_phase);

  mockMillis = 130;
  updateTouch(false);  // DOWN → LIFT_PENDING again
  mockMillis = 130 + LIFT_TIMEOUT_MS + 1;
  int result = updateTouch(false);
  TEST_ASSERT_EQUAL(1, result);  // NOW it classifies
}

void test_cooldown_blocks_input(void) {
  resetTouch();
  s_cooldownUntilMs = 500;
  mockMillis = 100;  // within cooldown
  int result = updateTouch(true);
  TEST_ASSERT_EQUAL(0, result);
  TEST_ASSERT_EQUAL(TOUCH_IDLE, s_phase);  // stays idle during cooldown
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_idle_to_down);
  RUN_TEST(test_down_to_lift_pending_on_release);
  RUN_TEST(test_lift_pending_returns_to_down_on_retouch);
  RUN_TEST(test_lift_pending_classifies_after_timeout);
  RUN_TEST(test_sample_gap_does_not_classify);
  RUN_TEST(test_cooldown_blocks_input);

  UNITY_END();
  return 0;
}
