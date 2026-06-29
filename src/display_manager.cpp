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

void drawSplash(const char* msg) {
  memset(g_framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  int32_t cx = EPD_WIDTH / 2 - 100;
  int32_t cy = EPD_HEIGHT / 2;
  writeln((GFXfont *)&FiraSans, msg, &cx, &cy, g_framebuffer);
}

uint8_t* framebuffer() { return g_framebuffer; }

} // namespace display_mgr
