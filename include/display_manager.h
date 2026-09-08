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
  uint8_t* framebuffer();
  constexpr int width()  { return EPD_WIDTH; }
  constexpr int height() { return EPD_HEIGHT; }
}
