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
  void drawSplash(const char* msg);
  uint8_t* framebuffer();
  constexpr int width()  { return EPD_WIDTH; }
  constexpr int height() { return EPD_HEIGHT; }
}
