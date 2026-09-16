#pragma once
#include <cstdint>
#include "epd_driver.h"

// Global framebuffer used by all drawing routines.
extern uint8_t *g_framebuffer;

namespace display_mgr {
  bool begin();
  void powerOn();
  void powerOff();

  // --- Choosing a refresh --------------------------------------------------
  //
  //   fullRefresh()     whole panel from the framebuffer (pair with
  //                     epd_clear at boot)
  //   partialRefresh()  CLEARED reflash of a row range — the brief
  //                     flash IS the reset
  //   ghostRefresh()    no-clear, add-ink-only push of a row range,
  //                     for transitional scrolling
  //   DiffRegion        tracked flash-free updates of a rect — the
  //                     workhorse for persistent UI (connection
  //                     screen, settings modal)
  //
  // The raw per-pass primitives (whiteInkRefresh / inkRefresh /
  // diffRefresh, below) are building blocks for calibration
  // (diff_test); apps should prefer DiffRegion.

  void fullRefresh();

  // CLEARED reflash — effectively a ROW-RANGE operation: the clear and
  // draw are full-width by design (sub-width clears leave drive
  // boundaries that darken the panel's edges over time), so x/w are
  // accepted for call-site uniformity but only y/h matter. The brief
  // flash is the reset: it wipes ghost residue and any charge buildup
  // from differential drives.
  void partialRefresh(int x, int y, int w, int h);

  // Fast no-clear variant: pushes framebuffer rows WITHOUT the clear
  // pass. Same row-range semantics as partialRefresh (full-width draw;
  // only y/h matter). No flashing and much quicker than partialRefresh
  // — but it can only ADD ink: the raw driver assumes the panel is
  // white when drawing (BLACK_ON_WHITE), so a white target produces no
  // drive and previously drawn content stays until the next full/
  // cleared refresh. Use only for transitional UI where that's
  // acceptable (timeline scrolling).
  void ghostRefresh(int x, int y, int w, int h);

  // Rect marking append-only or unchanged content inside a diff
  // region: existing ink is never re-driven, new ink is drawn in, and
  // pixels only erase if the content actually shrank/disappeared.
  struct PersistRect { int x, y, w, h; };

  // Tracked rect with flash-free differential updates. Owns the
  // "previous frame" record for its rows; drives only pixels inside
  // the rect (everything outside is never driven).
  struct DiffRegion {
    int x = 0, y = 0, w = 0, h = 0;   // tracked rect (panel coords)

    // Allocate the prev-frame record for the rect. Idempotent — safe to
    // call again (e.g. every time a modal opens); re-allocates only if
    // the rect changed or the buffer is missing. Returns false on
    // allocation failure — the caller should fall back to cleared
    // refreshes (partialRefresh).
    bool begin(int rx, int ry, int rw, int rh);

    // Record "the panel currently shows the framebuffer here" — call
    // after a cleared/full refresh of the area, or after inking content
    // directly (e.g. conn_screen's initial binary label).
    void syncFromFb();

    // One gentle white pass over the rect; records the rect as white.
    // Use when taking over an area whose panel state you can't track
    // (ghost residue, unknown ink). Pixels outside the rect are never
    // driven. The wipe is nibble-exact — only the rect's columns are
    // driven, even for odd x or w.
    void wipeWhite();

    // Flash-free full-swap of the whole rect: one white pass erases all
    // previous ink, two ink passes draw all current framebuffer ink,
    // the record is updated. `persist`: append-only or UNCHANGED content
    // rects (panel coords, e.g. a progress bar) whose existing ink is
    // never re-driven — pixels inside erase only if the content shrank.
    void update(const PersistRect* persist = nullptr, int persistCount = 0);

    // update() restricted to the row range [row, row+count) of the
    // region (REGION-RELATIVE rows) — cheaper for small edits (a single
    // settings row). Clamps count to the region.
    void updateRows(int row, int count,
                    const PersistRect* persist = nullptr, int persistCount = 0);

    uint8_t* prev = nullptr;  // full-width lines for rows [y, y+h)
  };

  // --- Raw differential-refresh primitives ---------------------------------
  // Per-pass building blocks (DiffRegion composes them); diff_test.cpp
  // drives them individually for hardware calibration.

  // Drive masked pixels toward WHITE ("white ink", WHITE_ON_WHITE mode).
  // maskBand holds the band's full-width rows; a nibble of 0 marks a
  // pixel to erase (driven hard), 15 leaves it untouched. ONE pass is
  // the correct dose on this panel — repeated passes build residual
  // charge and the erased pixels relax back toward their old state.
  void whiteInkRefresh(int y, int h, const uint8_t* maskBand, int passes);

  // Draw solid 1-bit black ink from the framebuffer over rows [y, y+h),
  // columns [x, x+w) only — pixels outside the rect receive no drive.
  // Single-polarity passes — no from-white dither assumptions; non-ink
  // pixels receive no drive at all. Two passes measured legibly dark;
  // ~8 reach fully solid black. Returns false if the scratch buffer
  // couldn't be allocated (caller should fall back to a cleared
  // refresh). With persistent rects (+prevBand), already-inked pixels
  // inside them are skipped — unchanged/append-only content keeps its
  // existing ink.
  bool inkRefresh(int x, int y, int w, int h, int passes,
                  const uint8_t* prevBand = nullptr,
                  const PersistRect* persist = nullptr, int persistCount = 0);

  // FULL-SWAP flash-free refresh of a rect's row band — the workflow
  // measured best-looking on hardware (total separation, uniform fresh
  // ink, fast): one white-ink pass erases ALL previous ink inside the
  // rect's columns (prevBand records what is physically on the panel),
  // no settle gap, then two solid ink passes draw ALL current content
  // (~half a second per update). Columns outside [x, x+w) are never
  // driven, and paper/background pixels are never driven either
  // (driving them greys the band over time). Updates prevBand to the
  // new content. Falls back to a cleared partial refresh if a scratch
  // buffer can't be allocated. prevBand indexing is band-relative
  // (row 0 = y), so an offset pointer into a larger full-width-line
  // buffer is valid.
  //
  // PERSISTENT RECTS (optional): regions of append-only or UNCHANGED
  // content (a progress bar; a label that didn't change this update).
  // Inside them nothing is re-driven — new ink is drawn in, and pixels
  // only erase if the content actually shrank. Everything else keeps
  // the validated full-swap behavior.
  void diffRefresh(int x, int y, int w, int h, uint8_t* prevBand,
                   const PersistRect* persist = nullptr, int persistCount = 0);
  uint8_t* framebuffer();
  constexpr int width()  { return EPD_WIDTH; }
  constexpr int height() { return EPD_HEIGHT; }
}
