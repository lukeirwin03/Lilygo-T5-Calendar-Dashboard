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
  // The EPD driver expects contiguous packed-pixel data for the sub-region.
  // Our framebuffer is row-major at EPD_WIDTH stride, so we must compact
  // the sub-region bytes into a temporary buffer.
  int rowBytes = w / 2;
  int bufSize = rowBytes * h;
  uint8_t* temp = (uint8_t*)ps_malloc(bufSize);
  if (!temp) {
    // Fallback to full refresh if allocation fails
    fullRefresh();
    return;
  }

  for (int row = 0; row < h; row++) {
    memcpy(temp + row * rowBytes,
           g_framebuffer + (y + row) * (EPD_WIDTH / 2) + x / 2,
           rowBytes);
  }

  Rect_t area;
  area.x = x;
  area.y = y;
  area.width = w;
  area.height = h;

  epd_poweron();
  // Clear the area to white first — e-paper partial refresh doesn't fully
  // transition pixels from gray to white, so gray compounds with each update.
  // epd_clear_area does a proper clear cycle on just this sub-region (brief
  // black-to-white flash), giving a clean slate before drawing new content.
  epd_clear_area(area);
  epd_draw_grayscale_image(area, temp);
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
