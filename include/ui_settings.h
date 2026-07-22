#pragma once
#include <cstdint>

namespace ui_settings {

enum TapResult {
  TAP_NONE,      // no meaningful tap
  TAP_CLOSE,     // exit settings screen
  TAP_FULL,      // full screen redraw needed
  TAP_PARTIAL    // partial redraw of a row/rows
};

// Render the full settings screen into the framebuffer.
void render();

// Handle a tap at (x, y). Returns what kind of redraw (if any) is required.
TapResult handleTap(int16_t x, int16_t y);

// Redraw any dirty UI elements. May be a no-op if handleTap already updated
// the framebuffer; callers use getDirtyRect() to know the refresh region.
void redrawDirty();

// Populate (x, y, w, h) with the bounding rectangle of the last dirty area.
void getDirtyRect(int& x, int& y, int& w, int& h);

} // namespace ui_settings
