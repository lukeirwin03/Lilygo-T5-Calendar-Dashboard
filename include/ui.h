#pragma once
#include <cstdint>
#include "dashboards/calendar_dashboard.h"

namespace ui {

enum Screen {
  SCREEN_WEEKLY,
  SCREEN_DAILY,
  SCREEN_SETTINGS,
  SCREEN_COUNT
};

void render();

// Toggle the settings modal open/closed (driven by the physical button).
void toggleSettings();
// Reset to the default view: weekly, centered on today, no event selected.
void resetToDefaultView();

// Feed parsed calendar events to the UI. The pointer must remain valid
// until the next call (ui does not copy the data).
void setEvents(const CalendarEvent* events, int count);

// Freshness timestamp of the currently-displayed data (UTC epoch from the
// payload's "updated" field; 0 = unknown). Set alongside setEvents().
void setLastUpdated(time_t epoch);
time_t getLastUpdated();   // UTC epoch of the displayed payload's "updated" field (0 = unknown)

// Request that the main loop reload the cached payload. Used when a setting
// that affects the event window (context_days) changes in the Settings modal.
// The main loop consumes it via consumeEventReloadRequest().
void requestEventReload();
bool consumeEventReloadRequest();

// Request that the main loop force a fresh MQTT pull. Set by the Settings
// modal's "Sync" button; consumed by the main loop via
// consumeManualRefreshRequest().
void requestManualRefresh();
bool consumeManualRefreshRequest();

// Touch input — called every loop iteration with the current touch state.
void updateTouch(bool isTouched, int16_t x, int16_t y);

// Returns true once after a touch interaction changed the UI and needs a redraw.
bool needsRender();

// Returns the refresh mode for the pending render.
// 0 = full refresh, 1 = partial settings refresh, 2 = partial daily refresh,
// 3 = no-clear ghost refresh of the focus column (timeline scroll),
// 4 = flash-free differential close of the settings modal,
// 5 = gray differential refresh of the whole screen (day nav).
int refreshMode();

// Populates the dirty rectangle for a partial settings refresh.
void getSettingsDirtyRect(int& x, int& y, int& w, int& h);

// Populates the dirty rectangle for a partial daily view refresh.
void getDailyDirtyRect(int& x, int& y, int& w, int& h);

// Populates the row range a timeline scroll can change (today's focus
// column), for the no-clear ghost refresh.
void getFocusGhostRect(int& x, int& y, int& w, int& h);

// --- Gray differential pushes ----------------------------------------------
// Each swaps freshly rendered content onto the panel with a flash-free,
// gray-capable differential refresh (white-ink erase of the previous
// ink + true-gray 4-bit draw). Call AFTER render() has drawn the new
// view into the framebuffer. The panel content is re-derived from a
// pre-render framebuffer snapshot each push (after any completed render
// push the panel matches the framebuffer), so no staleness tracking is
// needed. Each returns false if the region's prev-frame record couldn't
// be allocated — the caller falls back to a legacy refresh.

// Gray-diff the whole screen (day nav).
bool pushViewGray();

// Gray-diff the daily event-list rect (detail open / back to list).
bool pushDailyDetailGray();

// Gray-diff the focus timeline column (scroll arrows).
bool pushFocusScrollGray();

// Scroll-cadence bookkeeping: call once per focus-scroll render (mode 3,
// GRAY_DIFF_ENABLED path). Counts scrolls since the last cleared reflash
// of the focus column; returns true when the cadence is hit (and resets
// the counter) — the caller should do a cleared reflash of the focus
// rect instead of another gray diff. Not counted when gray diff is off
// (legacy ghost behavior is unchanged).
bool focusScrollFlashDue();

// Day-nav cadence bookkeeping: call once per day-nav render (mode 5,
// GRAY_DIFF_ENABLED path). Counts day navs since the last full flash;
// returns true when the cadence is hit (and resets the counter) — the
// caller should do a full cleared flash instead of another whole-screen
// gray diff. Not counted when gray diff is off (mode 5 is unreachable
// then, so the legacy behavior is unchanged).
bool navFlashDue();

} // namespace ui
