#pragma once
#include <cstdint>
#include "epd_driver.h"

// Global framebuffer used by all drawing routines.
extern uint8_t *g_framebuffer;

namespace display_mgr {
  bool begin();
  void powerOn();
  void powerOff();
  void fullRefresh();
  void partialRefresh(int x, int y, int w, int h);
  // Fast no-clear variant: pushes framebuffer rows WITHOUT the clear
  // pass. No flashing and much quicker than partialRefresh — but it can
  // only ADD ink: the raw driver assumes the panel is white when drawing
  // (BLACK_ON_WHITE), so a white target produces no drive and previously
  // drawn content stays until the next full/cleared refresh. Use only
  // for transitional UI where that's acceptable (timeline scrolling).
  void ghostRefresh(int x, int y, int w, int h);

  // --- Flash-free differential refresh primitives -----------------------
  // Both operate on full-width 4-bit row bands ([y, y+h)) and never run a
  // clear cycle. diffRefresh() composes them; diff_test.cpp drives them
  // individually for hardware calibration.

  // Drive masked pixels toward WHITE ("white ink", WHITE_ON_WHITE mode).
  // maskBand holds the band's rows; a nibble of 0 marks a pixel to erase
  // (driven hard), 15 leaves it untouched. ONE pass is the correct dose
  // on this panel — repeated passes build residual charge and the erased
  // pixels relax back toward their old state.
  void whiteInkRefresh(int y, int h, const uint8_t* maskBand, int passes);

  // Rect marking append-only or unchanged content inside a diff band:
  // existing ink is never re-driven, new ink is drawn in, and pixels
  // only erase if the content actually shrank/disappeared.
  struct PersistRect { int x, y, w, h; };

  // Draw solid 1-bit black ink from the framebuffer over the band.
  // Single-polarity passes — no from-white dither assumptions; non-ink
  // pixels receive no drive at all. Two passes measured legibly dark;
  // ~8 reach fully solid black. Returns false if the scratch buffer
  // couldn't be allocated (caller should fall back to a cleared refresh).
  // With persistent rects (+prevBand), already-inked pixels inside them
  // are skipped — unchanged/append-only content keeps its existing ink.
  bool inkRefresh(int y, int h, int passes,
                  const uint8_t* prevBand = nullptr,
                  const PersistRect* persist = nullptr, int persistCount = 0);

  // FULL-SWAP flash-free refresh of a row band — the workflow measured
  // best-looking on hardware (total separation, uniform fresh ink, fast):
  // one white-ink pass erases ALL previous ink (prevBand records what is
  // physically on the panel), no settle gap, then two solid ink passes
  // draw ALL current content (~half a second per update). Paper/
  // background pixels are never driven (driving them greys the band over
  // time). Updates prevBand to the new content. Falls back to a cleared
  // partial refresh if a scratch buffer can't be allocated.
  //
  // PERSISTENT RECTS (optional): regions of append-only or UNCHANGED
  // content (a progress bar; a label that didn't change this update).
  // Inside them nothing is re-driven — new ink is drawn in, and pixels
  // only erase if the content actually shrank. Everything else keeps the
  // validated full-swap behavior.
  void diffRefresh(int y, int h, uint8_t* prevBand,
                   const PersistRect* persist = nullptr, int persistCount = 0);
  uint8_t* framebuffer();
  constexpr int width()  { return EPD_WIDTH; }
  constexpr int height() { return EPD_HEIGHT; }
}
