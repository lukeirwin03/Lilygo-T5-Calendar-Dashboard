#include "display_manager.h"
#include "config.h"
#include <Arduino.h>
#include <cstring>

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

void ghostRefresh(int x, int y, int w, int h) {
  // Same row-copy strategy as partialRefresh (full-width lines to dodge
  // the driver's white-padding fast path), but WITHOUT epd_clear_area.
  // Fast and flicker-free — but it can only ADD ink: the raw driver
  // assumes the panel is white when drawing, so pixels whose target is
  // white receive no drive at all and old content stays at full
  // strength. Intended for transitional UI (timeline scroll) where a
  // later full refresh resets the panel.
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

  Rect_t drawArea;
  drawArea.x = 0;
  drawArea.y = y;
  drawArea.width = EPD_WIDTH;
  drawArea.height = h;

  epd_poweron();
  epd_draw_grayscale_image(drawArea, temp);
  epd_poweroff();

  free(temp);
}

// Row drive time for the 1-bit ink passes (notepad-tuned).
static constexpr int32_t INK_TIME_US = 200;

// True if (x, y) lies inside any of the persistent rects.
static bool inAnyPersist(const PersistRect* rects, int n, int x, int y) {
  for (int i = 0; i < n; i++) {
    if (x >= rects[i].x && x < rects[i].x + rects[i].w &&
        y >= rects[i].y && y < rects[i].y + rects[i].h) {
      return true;
    }
  }
  return false;
}

void whiteInkRefresh(int y, int h, const uint8_t* maskBand, int passes) {
  // Drive masked pixels toward WHITE via the driver's WHITE_ON_WHITE
  // "white ink" mode: in the grayscale LUT, a black data value drives
  // that pixel toward white while a white value produces no drive at
  // all. Full-width rows (the driver's sub-region path pads with white,
  // which is a no-op here but keeps the fast path).
  Rect_t area;
  area.x = 0;
  area.y = y;
  area.width = EPD_WIDTH;
  area.height = h;
  epd_poweron();
  for (int p = 0; p < passes; p++) {
    // Driver signature isn't const-correct; it never writes the buffer.
    epd_draw_image(area, const_cast<uint8_t*>(maskBand), WHITE_ON_WHITE);
  }
  epd_poweroff();
}

bool inkRefresh(int y, int h, int passes,
                const uint8_t* prevBand,
                const PersistRect* persist, int persistCount) {
  // Draw solid 1-bit black ink from the framebuffer over the band.
  // Packing follows the driver's 1-bit conventions (reverse-engineered
  // in the notepad project): 8 pixels/byte with the LEFTMOST pixel in
  // the LOW bit, and each byte written to index (b ^ 1) — the driver's
  // calc_epd_input_1bpp puts the first byte of each 16-pixel group on
  // the wrong physical half, so adjacent byte pairs are pre-swapped.
  // Non-ink pixels receive no drive.
  //
  // With persistent rects (+prevBand), pixels inside them that were
  // already ink are skipped — unchanged/append-only content keeps its
  // existing ink untouched.
  //
  // The frame MUST span the full panel height: the 1-bit path's skip_row
  // pipeline bookkeeping shifts sub-height frames' ink by a row relative
  // to the 4-bit path (notepad finding, re-confirmed on this build as
  // offset erase residue on the connection screen). Rows outside the
  // band are zero in the buffer = no drive, so only the frame time is
  // paid (~100 ms/pass), not panel wear.
  static constexpr uint8_t INK_MAX = 12;   // nibble <= this counts as ink
  bool usePersist = persist != nullptr && persistCount > 0 && prevBand != nullptr;
  int fullLineBytes = EPD_WIDTH / 2;
  int bytesPerRow = EPD_WIDTH / 8;
  uint8_t* ink = (uint8_t*)ps_malloc((size_t)bytesPerRow * EPD_HEIGHT);
  if (!ink) return false;
  memset(ink, 0, (size_t)bytesPerRow * EPD_HEIGHT);
  for (int row = 0; row < h; row++) {
    const uint8_t* cur = g_framebuffer + (y + row) * fullLineBytes;
    const uint8_t* prev = usePersist ? prevBand + row * fullLineBytes : nullptr;
    uint8_t* rowPtr = ink + (y + row) * bytesPerRow;
    int absY = y + row;
    for (int b = 0; b < bytesPerRow; b++) {
      uint8_t byte = 0;
      for (int bit = 0; bit < 8; bit++) {
        int srcX = b * 8 + bit;
        uint8_t nib = (srcX & 1) ? (cur[srcX / 2] >> 4) & 0x0F
                                 : cur[srcX / 2] & 0x0F;
        if (nib > INK_MAX) continue;
        if (usePersist && inAnyPersist(persist, persistCount, srcX, absY)) {
          uint8_t prevNib = (srcX & 1) ? (prev[srcX / 2] >> 4) & 0x0F
                                       : prev[srcX / 2] & 0x0F;
          if (prevNib <= INK_MAX) continue;   // already inked — leave it alone
        }
        byte |= (1 << bit);
      }
      rowPtr[b ^ 1] = byte;
    }
  }

  Rect_t area;
  area.x = 0;
  area.y = 0;
  area.width = EPD_WIDTH;
  area.height = EPD_HEIGHT;
  epd_poweron();
  for (int p = 0; p < passes; p++) {
    epd_draw_frame_1bit(area, ink, BLACK_ON_WHITE, INK_TIME_US);
  }
  epd_poweroff();
  free(ink);
  return true;
}

void diffRefresh(int y, int h, uint8_t* prevBand,
                 const PersistRect* persist, int persistCount) {
  // FULL-SWAP flash-free update — the workflow measured best on hardware
  // (diff-test screen: quality rows + speed lab):
  //   1. ONE white-ink pass over ALL previous ink — total separation:
  //      nothing old remains near the new content, and every pixel of
  //      the new content gets identical fresh drive (uniform ink). x1
  //      only: repeated passes build residual charge and the erased
  //      pixels relax back (fade-in rebound).
  //   2. NO settle gap — gap-0 swaps measured clean (RAPID row: six
  //      back-to-back gap-0 swaps, legible, artifact-free).
  //   3. Two solid ink passes over ALL current ink — speed-lab-validated
  //      legibility (~8 passes for fully solid black if ever needed).
  // Paper/background pixels are never driven (the whole-band WIPE
  // approach greyed the background over time); prevBand supplies what
  // ink is physically on the panel for the erase mask.
  //
  // PERSISTENT RECTS (optional): regions of append-only or UNCHANGED
  // content (a progress bar; a label that didn't change this update).
  // Inside them nothing is re-driven — new ink is drawn in, and pixels
  // only erase if the content actually shrank. Everything else keeps the
  // validated full-swap behavior.
  static constexpr uint8_t INK_MAX = 12;   // nibble <= this counts as ink
  static constexpr int     ERASE_PASSES  = 1;   // x1 validated; x2+ rebounds
  static constexpr int     ERASE_SETTLE_MS = 0; // gap-0 validated on hardware
  static constexpr int     INK_PASSES   = 2;    // speed-lab-validated legibility

  int fullLineBytes = EPD_WIDTH / 2;
  bool usePersist = persist != nullptr && persistCount > 0;

  // Fallback path: cleared refresh + prev update (always correct, flashes).
  auto fallbackCleared = [&]() {
    partialRefresh(0, y, EPD_WIDTH, h);
    for (int row = 0; row < h; row++) {
      memcpy(prevBand + row * fullLineBytes,
             g_framebuffer + (y + row) * fullLineBytes,
             fullLineBytes);
    }
  };

  uint8_t* temp = (uint8_t*)ps_malloc(fullLineBytes * h);
  if (!temp) {
    fallbackCleared();
    return;
  }

  // Erase mask: ALL previous ink — except inside persistent rects, where
  // only shrinking pixels (prev ink, now paper) are erased.
  bool anyErase = false;
  int erasePx = 0, inkPx = 0;
  for (int row = 0; row < h; row++) {
    const uint8_t* prev = prevBand + row * fullLineBytes;
    const uint8_t* cur  = g_framebuffer + (y + row) * fullLineBytes;
    uint8_t* out = temp + row * fullLineBytes;
    int absY = y + row;
    for (int b = 0; b < fullLineBytes; b++) {
      uint8_t prevHi = prev[b] >> 4;
      uint8_t prevLo = prev[b] & 0x0F;
      uint8_t curHi  = cur[b] >> 4;
      uint8_t curLo  = cur[b] & 0x0F;
      bool persistHi = usePersist && inAnyPersist(persist, persistCount, b * 2 + 1, absY);
      bool persistLo = usePersist && inAnyPersist(persist, persistCount, b * 2, absY);
      uint8_t hi = (prevHi <= INK_MAX && (!persistHi || curHi > INK_MAX)) ? 0x0 : 0xF;
      uint8_t lo = (prevLo <= INK_MAX && (!persistLo || curLo > INK_MAX)) ? 0x0 : 0xF;
      out[b] = (hi << 4) | lo;
      if (out[b] != 0xFF) {
        anyErase = true;
        erasePx += (hi == 0x0) + (lo == 0x0);
      }
      inkPx += (curHi <= INK_MAX) + (curLo <= INK_MAX);
    }
  }
  Serial.printf("[diff] y=%d h=%d: erase %d px (x1), ink %d px, %d persist rect(s)\n",
                y, h, erasePx, inkPx, usePersist ? persistCount : 0);

  // 1) Erase (one pass over the mask).
  if (anyErase) {
    whiteInkRefresh(y, h, temp, ERASE_PASSES);
    if (ERASE_SETTLE_MS > 0) delay(ERASE_SETTLE_MS);
  }
  free(temp);

  // 2) Ink: everything except already-inked persistent-rect pixels.
  if (!inkRefresh(y, h, INK_PASSES,
                  usePersist ? prevBand : nullptr,
                  persist, persistCount)) {
    fallbackCleared();
  }

  // prev = new, for the next differential update.
  for (int row = 0; row < h; row++) {
    memcpy(prevBand + row * fullLineBytes,
           g_framebuffer + (y + row) * fullLineBytes,
           fullLineBytes);
  }
}

uint8_t* framebuffer() { return g_framebuffer; }

} // namespace display_mgr
