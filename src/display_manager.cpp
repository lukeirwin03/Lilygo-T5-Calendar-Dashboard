#include "display_manager.h"
#include "config.h"
#include <Arduino.h>
#include <cstring>
#include "firasans.h"

uint8_t *g_framebuffer = nullptr;

namespace display_mgr {

bool begin() {
  if (!g_framebuffer) {
    g_framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!g_framebuffer) {
      Serial.println("[display] PSRAM alloc failed, falling back to internal RAM");
      g_framebuffer = (uint8_t *)calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    }
    if (!g_framebuffer) {
      Serial.println("[display] Framebuffer alloc failed!");
      return false;
    }
  }
  memset(g_framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  epd_init();
  // Initial clear cycle — the panel needs this on first power-up to
  // reach a known white state. Skipping it can leave the factory image.
  epd_poweron();
  epd_clear();
  epd_poweroff_all();
  return true;
}

void powerOn()  { epd_poweron(); }
void powerOff() { epd_poweroff_all(); }

void fullRefresh() {
  epd_poweron();
  epd_draw_grayscale_image(epd_full_screen(), g_framebuffer);
  epd_poweroff();
}

void partialRefresh(int x, int y, int w, int h) {
  // Align x and w to 4-pixel boundaries for the clear operation.
  int alignedX = (x / 4) * 4;
  w += (x - alignedX);
  w = ((w + 3) / 4) * 4;
  x = alignedX;

  // Copy FULL-WIDTH lines from the framebuffer. The EPD driver's
  // provide_out() function pads sub-region lines with white (255), which
  // causes ghosting at the left/right edges over multiple partial refreshes.
  // By drawing at full width, we use the fast path that sends actual
  // framebuffer content for every pixel — no white padding.
  int fullLineBytes = EPD_WIDTH / 2;
  int bufSize = fullLineBytes * h;
  uint8_t* temp = (uint8_t*)ps_malloc(bufSize);
  if (!temp) {
    fullRefresh();
    return;
  }

  for (int row = 0; row < h; row++) {
    memcpy(temp + row * fullLineBytes,
           g_framebuffer + (y + row) * fullLineBytes,
           fullLineBytes);
  }

  // Clear area: full width to match the draw area. A sub-region clear
  // creates a physical driving boundary on the e-paper panel that causes
  // charge leakage into adjacent pixels, producing darkening strips at the
  // left/right edges. Full-width clear eliminates the boundary.
  // The calendar widget will flash briefly during the clear cycle but is
  // immediately redrawn by the draw operation.
  Rect_t clearArea;
  clearArea.x = 0;
  clearArea.y = y;
  clearArea.width = EPD_WIDTH;
  clearArea.height = h;

  // Draw area: full width, same row range. This uses the provide_out()
  // fast path (area.width == EPD_WIDTH && area.x == 0) which avoids
  // white-padding edge artifacts.
  Rect_t drawArea;
  drawArea.x = 0;
  drawArea.y = y;
  drawArea.width = EPD_WIDTH;
  drawArea.height = h;

  epd_poweron();
  epd_clear_area(clearArea);
  epd_draw_grayscale_image(drawArea, temp);
  epd_poweroff();

  free(temp);
}

void drawSplash(const char* msg) {
  memset(g_framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  int32_t cx = EPD_WIDTH / 2 - 100;
  int32_t cy = EPD_HEIGHT / 2;
  writeln((GFXfont *)&FiraSans, msg, &cx, &cy, g_framebuffer);
}

uint8_t* framebuffer() { return g_framebuffer; }

} // namespace display_mgr
