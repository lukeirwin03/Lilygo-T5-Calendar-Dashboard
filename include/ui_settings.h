#pragma once
#include <cstdint>

// Settings modal drawn over the current view. The modal renders BINARY
// (black/white only) through the 1-bit ink path of the flash-free
// differential refresh: the ink predicate thresholds any gray nibble
// (<= 12) to solid black, so gray fills are meaningless here — hence
// the pure black-and-white styling (e.g. inverted selected rows).

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

// Populate (x, y, w, h) with the bounding rectangle of the last dirty area.
void getDirtyRect(int& x, int& y, int& w, int& h);

// Mark the whole modal as needing redraw (called by ui when opening the modal).
void markFullRedraw();

// Lifecycle: open wipes ONLY the modal's columns (the strips of
// underlying view beside the modal keep their ink — no white flash
// band); in-modal updates (tab switch / row edits) are column-limited
// diffs that never drive the strips; close is a cleared reflash of the
// modal's row range with fresh data (see closeReflash).

// Push the (re)rendered modal to the panel flash-free. Call AFTER
// ui::render() drew the modal (refresh mode REFRESH_PARTIAL_SETTINGS).
// Returns false if the prev buffer couldn't be allocated — caller
// should fall back to the old flashing partialRefresh.
bool diffPush();

// Close-time reflash: a CLEARED partial refresh of the modal's row
// range with the freshly re-rendered view. Unlike the diff pushes,
// this fully resets those pixels (any charge buildup from the
// repeated diff drives, plus ghost residue, is wiped) and pulls the
// current data. Call AFTER ui::render() restored the underlying
// screen. (partialRefresh clears full-width rows by design — a
// sub-width clear leaves drive boundaries that darken the panel's
// edges over time; see its comments in display_manager.cpp.)
void closeReflash();

} // namespace ui_settings
