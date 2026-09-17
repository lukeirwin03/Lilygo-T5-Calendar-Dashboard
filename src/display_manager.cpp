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
  // x/w are intentionally unused (row-range semantics): the clear and
  // draw areas below are full-width by design, so only y/h matter.

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

bool inkRefresh(int x, int y, int w, int h, int passes,
                const uint8_t* prevBand,
                const PersistRect* persist, int persistCount) {
  // Draw solid 1-bit black ink from the framebuffer over rows [y, y+h),
  // columns [x, x+w) only — pixels outside the rect receive no drive.
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
  int xEnd = x + w;
  uint8_t* ink = (uint8_t*)ps_malloc((size_t)bytesPerRow * EPD_HEIGHT);
  if (!ink) return false;
  memset(ink, 0, (size_t)bytesPerRow * EPD_HEIGHT);
  for (int row = 0; row < h; row++) {
    const uint8_t* cur = g_framebuffer + (y + row) * fullLineBytes;
    const uint8_t* prev = usePersist ? prevBand + row * fullLineBytes : nullptr;
    uint8_t* rowPtr = ink + (y + row) * bytesPerRow;
    int absY = y + row;
    for (int b = x / 8; b <= (xEnd - 1) / 8; b++) {
      uint8_t byte = 0;
      for (int bit = 0; bit < 8; bit++) {
        int srcX = b * 8 + bit;
        if (srcX < x || srcX >= xEnd) continue;   // outside the rect's columns
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

// Build the white-ink erase mask for rows [y, y+h), columns [x, x+w):
// nibble 0 = drive that pixel to white, 0xF = no drive. ALL previous ink
// (nibble <= inkMax) inside the rect's columns is erased — except inside
// persistent rects, where only shrinking pixels (prev ink, now paper) are
// erased. Columns outside [x, x+w) stay no-drive. prevBand is band-relative
// (row 0 = y). inkMax sets what counts as ink: the binary diffRefresh path
// thresholds at 12, the gray updateGray path must erase up to 14. Also
// reports whether anything needs erasing, the erased pixel count, and —
// for the caller's serial log — the current ink pixel count (same
// inkMax predicate; nullable). Returns nullptr on alloc failure.
static uint8_t* buildEraseMask(int x, int y, int w, int h,
                               const uint8_t* prevBand,
                               const PersistRect* persist, int persistCount,
                               int inkMax,
                               bool* anyEraseOut, int* erasePxOut,
                               int* inkPxOut = nullptr) {
  int fullLineBytes = EPD_WIDTH / 2;
  bool usePersist = persist != nullptr && persistCount > 0;
  int xEnd = x + w;

  uint8_t* temp = (uint8_t*)ps_malloc(fullLineBytes * h);
  if (!temp) return nullptr;
  // Default: no drive anywhere; only the rect's columns get computed.
  memset(temp, 0xFF, (size_t)fullLineBytes * h);

  bool anyErase = false;
  int erasePx = 0, inkPx = 0;
  for (int row = 0; row < h; row++) {
    const uint8_t* prev = prevBand + row * fullLineBytes;
    const uint8_t* cur  = g_framebuffer + (y + row) * fullLineBytes;
    uint8_t* out = temp + row * fullLineBytes;
    int absY = y + row;
    for (int b = x / 2; b <= (xEnd - 1) / 2; b++) {
      uint8_t prevHi = prev[b] >> 4;
      uint8_t prevLo = prev[b] & 0x0F;
      uint8_t curHi  = cur[b] >> 4;
      uint8_t curLo  = cur[b] & 0x0F;
      bool hiIn = (b * 2 + 1 >= x) && (b * 2 + 1 < xEnd);
      bool loIn = (b * 2 >= x) && (b * 2 < xEnd);
      bool persistHi = usePersist && inAnyPersist(persist, persistCount, b * 2 + 1, absY);
      bool persistLo = usePersist && inAnyPersist(persist, persistCount, b * 2, absY);
      uint8_t hi = (hiIn && prevHi <= inkMax && (!persistHi || curHi > inkMax)) ? 0x0 : 0xF;
      uint8_t lo = (loIn && prevLo <= inkMax && (!persistLo || curLo > inkMax)) ? 0x0 : 0xF;
      out[b] = (hi << 4) | lo;
      if (out[b] != 0xFF) {
        anyErase = true;
        erasePx += (hi == 0x0) + (lo == 0x0);
      }
      inkPx += (hiIn && curHi <= inkMax) + (loIn && curLo <= inkMax);
    }
  }
  *anyEraseOut = anyErase;
  *erasePxOut = erasePx;
  if (inkPxOut) *inkPxOut = inkPx;
  return temp;
}

void diffRefresh(int x, int y, int w, int h, uint8_t* prevBand,
                 const PersistRect* persist, int persistCount) {
  // FULL-SWAP flash-free update of the rect's row band — the workflow
  // measured best on hardware (diff-test screen: quality rows + speed
  // lab):
  //   1. ONE white-ink pass over ALL previous ink — total separation:
  //      nothing old remains near the new content, and every pixel of
  //      the new content gets identical fresh drive (uniform ink). x1
  //      only: repeated passes build residual charge and the erased
  //      pixels relax back (fade-in rebound).
  //   2. NO settle gap — gap-0 swaps measured clean (RAPID row: six
  //      back-to-back gap-0 swaps, legible, artifact-free).
  //   3. Two solid ink passes over ALL current ink — speed-lab-validated
  //      legibility (~8 passes for fully solid black if ever needed).
  // Columns outside [x, x+w) are never driven, and paper/background
  // pixels are never driven either (the whole-band WIPE approach greyed
  // the background over time); prevBand supplies what ink is physically
  // on the panel for the erase mask.
  //
  // PERSISTENT RECTS (optional): regions of append-only or UNCHANGED
  // content (a progress bar; a label that didn't change this update).
  // Inside them nothing is re-driven — new ink is drawn in, and pixels
  // only erase if the content actually shrank. Everything else keeps the
  // validated full-swap behavior.
  static constexpr int ERASE_PASSES    = 1;   // x1 validated; x2+ rebounds
  static constexpr int ERASE_SETTLE_MS = 0;   // gap-0 validated on hardware
  static constexpr int INK_PASSES      = 2;   // speed-lab-validated legibility

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

  // Erase mask: ALL previous ink inside the rect's columns — except
  // inside persistent rects, where only shrinking pixels (prev ink,
  // now paper) are erased. Columns outside [x, x+w) stay no-drive.
  // inkMax 12: this binary path's panel ink is thresholded ≤ 12 anyway
  // (INK_MAX in inkRefresh), so no shade above 12 can be on the panel
  // through it.
  bool anyErase = false;
  int erasePx = 0, inkPx = 0;
  uint8_t* temp = buildEraseMask(x, y, w, h, prevBand, persist, persistCount, 12,
                                 &anyErase, &erasePx, &inkPx);
  if (!temp) {
    fallbackCleared();
    return;
  }
  unsigned long t0 = millis();   // total erase+ink duration, logged below

  // 1) Erase (one pass over the mask).
  if (anyErase) {
    whiteInkRefresh(y, h, temp, ERASE_PASSES);
    if (ERASE_SETTLE_MS > 0) delay(ERASE_SETTLE_MS);
  }
  free(temp);

  // 2) Ink: everything except already-inked persistent-rect pixels.
  if (!inkRefresh(x, y, w, h, INK_PASSES,
                  usePersist ? prevBand : nullptr,
                  persist, persistCount)) {
    fallbackCleared();
  }

  Serial.printf("[diff] y=%d h=%d: erase %d px (x1), ink %d px, %d persist rect(s), %lu ms\n",
                y, h, erasePx, inkPx, usePersist ? persistCount : 0, millis() - t0);

  // prev = new, for the next differential update.
  for (int row = 0; row < h; row++) {
    memcpy(prevBand + row * fullLineBytes,
           g_framebuffer + (y + row) * fullLineBytes,
           fullLineBytes);
  }
}

// --- DiffRegion ------------------------------------------------------------

bool DiffRegion::begin(int rx, int ry, int rw, int rh) {
  int fullLineBytes = EPD_WIDTH / 2;
  if (prev && x == rx && y == ry && w == rw && h == rh) {
    return true;   // same rect — keep the existing record
  }
  free(prev);
  prev = nullptr;
  x = rx; y = ry; w = rw; h = rh;
  // Full-width lines so the band-relative prev logic in diffRefresh
  // works on the region's rows unchanged.
  prev = (uint8_t*)ps_malloc((size_t)fullLineBytes * h);
  if (!prev) {
    x = y = w = h = 0;   // zeroed rect so a later begin() retries
    return false;
  }
  memset(prev, 0xFF, (size_t)fullLineBytes * h);
  return true;
}

void DiffRegion::syncFromFb() {
  if (!prev) return;
  int fullLineBytes = EPD_WIDTH / 2;
  memcpy(prev, g_framebuffer + (size_t)y * fullLineBytes,
         (size_t)fullLineBytes * h);
}

void DiffRegion::wipeWhite() {
  if (!prev) return;
  int fullLineBytes = EPD_WIDTH / 2;
  size_t bandBytes = (size_t)fullLineBytes * h;
  uint8_t* mask = (uint8_t*)ps_malloc(bandBytes);
  if (!mask) return;   // can't build the mask — leave panel and record alone
  memset(mask, 0xFF, bandBytes);   // default: no drive anywhere
  for (int row = 0; row < h; row++) {
    // Zero exactly the nibbles for columns [x, x+w); nibbles outside
    // the rect stay no-drive. Byte b holds pixel 2b (low nibble) and
    // 2b+1 (high nibble).
    uint8_t* line = mask + (size_t)row * fullLineBytes;
    int b0 = x / 2, b1 = (x + w - 1) / 2;
    if (b1 > b0 + 1)
      memset(line + b0 + 1, 0x00, (size_t)(b1 - b0 - 1));
    uint8_t first = (x & 1) ? 0x0F : 0x00;        // keep low nibble if x odd
    uint8_t last = ((x + w) & 1) ? 0xF0 : 0x00;   // keep high nibble if right edge odd
    if (b0 == b1) {
      line[b0] &= first | last;   // one byte spans both edges — OR the keeps
    } else {
      line[b0] &= first;
      line[b1] &= last;
    }
  }
  whiteInkRefresh(y, h, mask, 1);
  free(mask);
  // Columns outside the rect are never consulted, so recording the
  // whole band as white is correct (and simpler).
  memset(prev, 0xFF, bandBytes);
}

void DiffRegion::update(const PersistRect* persist, int persistCount) {
  if (!prev) return;
  diffRefresh(x, y, w, h, prev, persist, persistCount);
}

void DiffRegion::updateRows(int row, int count,
                            const PersistRect* persist, int persistCount) {
  if (!prev) return;
  if (row < 0) { count += row; row = 0; }
  if (row + count > h) count = h - row;
  if (count < 1) return;
  int fullLineBytes = EPD_WIDTH / 2;
  diffRefresh(x, y + row, w, count, prev + (size_t)row * fullLineBytes,
              persist, persistCount);
}

void DiffRegion::updateGray() {
  if (!prev) return;
  int fullLineBytes = EPD_WIDTH / 2;

  // Fallback path: cleared refresh + prev resync (always correct,
  // flashes) — mirrors diffRefresh's fallbackCleared.
  auto fallbackCleared = [&]() {
    partialRefresh(0, y, EPD_WIDTH, h);
    syncFromFb();
  };

  unsigned long t0 = millis();   // erase+draw duration, logged below

  // 1) Erase ALL previous ink in the rect (any shade) — one white pass
  //    over the same mask diffRefresh builds. No persistent rects here.
  //    inkMax 14: the 4-bit draw (BLACK_ON_WHITE) drives ALL non-white
  //    values, so shades 13/14 are real panel ink here — the erase must
  //    treat all of them as ink or light-gray content would never be
  //    erased. Pure paper (15) stays untouched per the never-drive-paper
  //    rule.
  bool anyErase = false;
  int erasePx = 0;
  uint8_t* mask = buildEraseMask(x, y, w, h, prev, nullptr, 0, 14,
                                 &anyErase, &erasePx);
  if (!mask) {
    fallbackCleared();
    return;
  }
  if (anyErase) whiteInkRefresh(y, h, mask, 1);
  free(mask);

  // 2) 4-bit draw of the new content. Full-width row buffer copied from
  //    the framebuffer with columns OUTSIDE the rect whited out (0xFF =
  //    no drive) so the draw touches only the rect's columns — the
  //    driver's sub-width area path pads with white in a way that has
  //    caused edge artifacts historically; full-width with whited-out
  //    strips keeps the proven fast path and guarantees no drive
  //    outside. Per row: memcpy the full fb line, then memset 0xFF over
  //    bytes [0, x/2) and [(x+w+1)/2, fullLineBytes). If x or w is odd
  //    the boundary byte keeps one fb nibble — accept it: that pixel
  //    sits at the rect's edge and gets the fb value, which is a no-op
  //    or a correct draw for a pixel the erase already treated as
  //    in-rect (current callers are even-aligned).
  uint8_t* temp = (uint8_t*)ps_malloc((size_t)fullLineBytes * h);
  if (!temp) {
    fallbackCleared();
    return;
  }
  int leftBytes = x / 2;               // bytes fully left of the rect
  int rightStart = (x + w + 1) / 2;    // first byte fully right of it
  for (int row = 0; row < h; row++) {
    uint8_t* line = temp + row * fullLineBytes;
    memcpy(line, g_framebuffer + (y + row) * fullLineBytes, fullLineBytes);
    memset(line, 0xFF, leftBytes);
    memset(line + rightStart, 0xFF, fullLineBytes - rightStart);
  }

  // 3) True-gray draw: the rect was just erased to white, which is
  //    exactly this path's from-white assumption.
  Rect_t area;
  area.x = 0;
  area.y = y;
  area.width = EPD_WIDTH;
  area.height = h;
  epd_poweron();
  epd_draw_image(area, temp, BLACK_ON_WHITE);
  epd_poweroff();
  free(temp);

  // 4) prev = new, for the next differential update.
  syncFromFb();

  Serial.printf("[graydiff] x=%d w=%d y=%d h=%d: erase %d px, %lu ms\n",
                x, w, y, h, erasePx, millis() - t0);
}

uint8_t* framebuffer() { return g_framebuffer; }
} // namespace display_mgr
