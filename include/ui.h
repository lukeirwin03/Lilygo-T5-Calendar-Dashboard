#pragma once
#include <cstdint>
#include "dashboards/calendar_dashboard.h"

namespace ui {

enum Screen {
  SCREEN_WEEKLY,
  SCREEN_DAILY,
  SCREEN_COUNT
};

void render();
void next();

// Feed parsed calendar events to the UI. The pointer must remain valid
// until the next call (ui does not copy the data).
void setEvents(const CalendarEvent* events, int count);

// Touch input — called every loop iteration with the current touch state.
void updateTouch(bool isTouched, int16_t x, int16_t y);

// Returns true once after a touch interaction changed the UI and needs a redraw.
bool needsRender();

} // namespace ui
