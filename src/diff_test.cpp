#include "diff_test.h"
#include "display_manager.h"
#include "epd_driver.h"
#include "config.h"
#include "fonts/MeltSwashes14pt7b.h"
#include "fonts/MeltSwashes16pt7b.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>

namespace diff_test {

// ---------------------------------------------------------------------------
// Palette / layout
// ---------------------------------------------------------------------------
static constexpr uint8_t C_BLACK = 0;
static constexpr uint8_t C_WHITE = 15;
static constexpr uint8_t EPD_BLACK = C_BLACK << 4;
static constexpr uint8_t EPD_WHITE = C_WHITE << 4;

static constexpr int ROW_H    = 64;
static constexpr int ROW_TOP  = 82;
static constexpr int ROW_COUNT = 6;
static constexpr int CELL_X   = 250;   // test ink lands right of the labels
static constexpr int CELL_W   = EPD_WIDTH - CELL_X - 10;

static const char* const kRowLabels[ROW_COUNT] = {
  "ERASE x1", "DIFF SWAP", "GAP 0", "GAP 100", "GAP 250", "RAPID x2",
};

static constexpr uint8_t INK_MAX = 12;   // must match display_manager

// Full-screen "previous frame" for the differential ops (PSRAM).
static uint8_t* s_prev = nullptr;

static int rowY(int r) { return ROW_TOP + r * ROW_H; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// Wait ms, returning true as soon as the button is pressed (exit signal).
static bool waitBtn(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (digitalRead(config::BUTTON_PIN) == LOW) return true;
    delay(20);
  }
  return digitalRead(config::BUTTON_PIN) == LOW;
}

static void fillCellWhite(int r, uint8_t* fb) {
  epd_fill_rect(CELL_X, rowY(r), CELL_W, ROW_H - 6, EPD_WHITE, fb);
}

// Draw text at the cell's fixed anchor (never moves — so ink/erase masks
// are exactly the glyphs that changed).
static void drawCellText(int r, uint8_t* fb, const char* str, int dots = 0) {
  int32_t x = CELL_X + 14;
  int32_t y = rowY(r) + 36;
  FontProperties props;
  props.fg_color = C_BLACK;
  props.bg_color = C_WHITE;
  props.flags = 0;
  props.fallback_glyph = 0;
  write_mode((GFXfont*)&MeltSwashes16, str, &x, &y, fb, BLACK_ON_WHITE, &props);
  if (dots > 0) {
    char dotsStr[4];
    snprintf(dotsStr, sizeof(dotsStr), "%.*s", dots, "...");
    int32_t lw = 0, lh = 0, x1 = 0, y1 = 0;
    int32_t mcx = x, mcy = y;
    get_text_bounds((GFXfont*)&MeltSwashes16, str, &mcx, &mcy, &x1, &y1, &lw, &lh, NULL);
    int32_t dx = x + lw + 10;
    write_mode((GFXfont*)&MeltSwashes16, dotsStr, &dx, &y, fb, BLACK_ON_WHITE, &props);
  }
}

// Copy the row's framebuffer rows into s_prev (the "as pushed" record).
static void syncPrev(int r) {
  int fullLineBytes = EPD_WIDTH / 2;
  memcpy(s_prev + rowY(r) * fullLineBytes,
         display_mgr::framebuffer() + rowY(r) * fullLineBytes,
         (size_t)fullLineBytes * ROW_H);
}

// Build an erase mask band for row r. `invert` selects the encoding:
//   false: erase pixels = 0x0, untouched = 0xF   (our LUT model)
//   true : erase pixels = 0xF, paper = 0x0       (the inverted hypothesis)
// Masked erase pixels are prev-ink that is now paper.
static void buildRowMask(int r, uint8_t* out, bool invert) {
  int fullLineBytes = EPD_WIDTH / 2;
  for (int row = 0; row < ROW_H; row++) {
    const uint8_t* prev = s_prev + (rowY(r) + row) * fullLineBytes;
    const uint8_t* cur  = display_mgr::framebuffer() + (rowY(r) + row) * fullLineBytes;
    uint8_t* o = out + row * fullLineBytes;
    for (int b = 0; b < fullLineBytes; b++) {
      uint8_t prevHi = prev[b] >> 4;
      uint8_t curHi  = cur[b] >> 4;
      uint8_t prevLo = prev[b] & 0x0F;
      uint8_t curLo  = cur[b] & 0x0F;
      bool eraseHi = (prevHi <= INK_MAX && curHi > INK_MAX);
      bool eraseLo = (prevLo <= INK_MAX && curLo > INK_MAX);
      uint8_t hi, lo;
      if (!invert) {
        hi = eraseHi ? 0x0 : 0xF;
        lo = eraseLo ? 0x0 : 0xF;
      } else {
        // Inverted encoding: erase targets 15, plain paper 0. Under our
        // model nothing visible happens (paper stays paper, targets are
        // "untouched"); if the LUT mapping is backwards, the targets —
        // and only they — erase.
        hi = eraseHi ? 0xF : (curHi > INK_MAX ? 0x0 : 0xF);
        lo = eraseLo ? 0xF : (curLo > INK_MAX ? 0x0 : 0xF);
      }
      o[b] = (hi << 4) | lo;
    }
  }
}

// Reset a cell to a known-clean state using ONLY the hardware-validated
// primitive (×1 masked white-ink erase): erase whatever the previous
// cycle left on the panel, settle, then ink the base word fresh.
//
// Without this, the base-word re-ink at the top of each cycle piles text
// over whatever the previous cycle's last update left — which made every
// row except ERASE x1 (the only one that ends its cycle erased) look
// like it was "writing over itself", masking the actual probe results.
static void resetCellClean(int r, uint8_t* fb, uint8_t* mask) {
  fillCellWhite(r, fb);
  buildRowMask(r, mask, false);
  display_mgr::whiteInkRefresh(rowY(r), ROW_H, mask, 1);
  delay(1000);   // same erase→ink gap as diffRefresh (observed-clean timing)
  drawCellText(r, fb, "Quixotic 42");
  display_mgr::inkRefresh(0, rowY(r), EPD_WIDTH, ROW_H, 8);
  syncPrev(r);
}

// Full-swap transition with PARAMETERIZED timing — the speed-test
// workhorse. Identical sequence to diffRefresh/resetCellClean (erase ALL
// prev ink ×1 → gap → ink ALL new content), but the settle gap and ink
// pass count are caller-controlled, and each phase is timed to the
// serial log.
static void speedSwap(int r, uint8_t* fb, uint8_t* mask, const char* word,
                      int gapMs, int inkPasses) {
  unsigned long t0 = millis();
  fillCellWhite(r, fb);
  buildRowMask(r, mask, false);          // cur is all white → mask = ALL prev ink
  display_mgr::whiteInkRefresh(rowY(r), ROW_H, mask, 1);
  unsigned long tErase = millis();
  if (gapMs > 0) delay(gapMs);
  unsigned long tGap = millis();
  drawCellText(r, fb, word);
  display_mgr::inkRefresh(0, rowY(r), EPD_WIDTH, ROW_H, inkPasses);
  unsigned long tInk = millis();
  syncPrev(r);
  Serial.printf("[difftest] row %d (%s): erase %lu ms + gap %d ms + ink x%d %lu ms = %lu ms total\n",
                r, kRowLabels[r], tErase - t0, gapMs, inkPasses,
                tInk - tGap, tInk - t0);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void run() {
  Serial.println("[difftest] differential refresh calibration — press button to exit");
  uint8_t* fb = display_mgr::framebuffer();
  int fullLineBytes = EPD_WIDTH / 2;

  // Static layout: title + row labels, pushed with one full refresh.
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  {
    int32_t x = 24, y = 40;
    FontProperties props;
    props.fg_color = C_BLACK; props.bg_color = C_WHITE;
    props.flags = 0; props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, "DIFF TEST — button exits", &x, &y,
               fb, BLACK_ON_WHITE, &props);
  }
  for (int r = 0; r < ROW_COUNT; r++) {
    int32_t x = 24, y = rowY(r) + 36;
    FontProperties props;
    props.fg_color = C_BLACK; props.bg_color = C_WHITE;
    props.flags = 0; props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes14, kRowLabels[r], &x, &y,
               fb, BLACK_ON_WHITE, &props);
  }
  display_mgr::powerOn();
  display_mgr::fullRefresh();
  display_mgr::powerOff();

  // Seed the full-screen "previous frame": panel == framebuffer now.
  s_prev = (uint8_t*)ps_malloc((size_t)fullLineBytes * EPD_HEIGHT);
  if (!s_prev) {
    Serial.println("[difftest] PSRAM alloc failed — skipping test screen");
    return;
  }
  memcpy(s_prev, fb, (size_t)fullLineBytes * EPD_HEIGHT);

  uint8_t* mask = (uint8_t*)ps_malloc((size_t)fullLineBytes * ROW_H);

  int cycle = 1;
  bool exit = false;
  while (!exit) {
    // --- 1) Clean reset of every cell (validated ×1 erase + fresh ink),
    //        so each probe below is judged from an identical clean state. ---
    Serial.printf("[difftest] cycle %d: clean reset + base ink\n", cycle);
    for (int r = 0; r < ROW_COUNT; r++) {
      resetCellClean(r, fb, mask);
    }
    if (waitBtn(2500)) break;

    // --- 2) ERASE x1 (reference winner) ---
    Serial.printf("[difftest] cycle %d: erase row 0 (1 pass)\n", cycle);
    fillCellWhite(0, fb);
    buildRowMask(0, mask, false);
    display_mgr::whiteInkRefresh(rowY(0), ROW_H, mask, 1);
    syncPrev(0);
    exit = waitBtn(1000);
    if (exit) break;

    // --- 3) DIFF SWAP: the shipping full-swap (1000 ms gap, ink x8) —
    //        the quality reference for the speed rows below. ---
    Serial.printf("[difftest] cycle %d: swap row 1 via diffRefresh (shipping timing)\n", cycle);
    fillCellWhite(1, fb);
    drawCellText(1, fb, (cycle % 2) ? "Alpha 7" : "Omega 7");
    display_mgr::diffRefresh(0, rowY(1), EPD_WIDTH, ROW_H,
                             s_prev + rowY(1) * fullLineBytes);
    exit = waitBtn(1500);
    if (exit) break;

    // --- 4) Speed ladder: full swaps at 0/100/250 ms gaps (ink x8).
    //        Judge each row's cleanliness vs row 1's 1000 ms reference. ---
    {
      static const int gaps[3] = {0, 100, 250};
      for (int i = 0; i < 3 && !exit; i++) {
        int r = 2 + i;
        Serial.printf("[difftest] cycle %d: speed row %d (gap %d ms)\n", cycle, r, gaps[i]);
        speedSwap(r, fb, mask, (cycle % 2) ? "Alpha 7" : "Omega 7", gaps[i], 8);
        exit = waitBtn(1200);
      }
    }
    if (exit) break;

    // --- 5) RAPID FIRE: row 5 hammers gap-0, 2-pass-ink swaps back to
    //        back — the floor of how fast a legible swap can go, and a
    //        charge-buildup stress test. Total time for 6 swaps reported. ---
    {
      Serial.printf("[difftest] cycle %d: rapid fire row 5 (gap 0, ink x2, 6 swaps)\n", cycle);
      unsigned long tStart = millis();
      for (int i = 0; i < 6; i++) {
        speedSwap(5, fb, mask, (i % 2) ? "Alpha 7" : "Omega 7", 0, 2);
      }
      Serial.printf("[difftest] rapid fire: 6 swaps in %lu ms (%lu ms avg)\n",
                    millis() - tStart, (millis() - tStart) / 6);
      exit = waitBtn(2000);
    }
    cycle++;
  }

  free(mask);
  free(s_prev);
  s_prev = nullptr;
  Serial.println("[difftest] button pressed — exiting");
}

} // namespace diff_test
