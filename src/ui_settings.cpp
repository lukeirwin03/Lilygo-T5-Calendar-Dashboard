#include "ui_settings.h"
#include "settings.h"
#include "battery.h"
#include "display_manager.h"
#include "epd_driver.h"
#include "fonts/MeltSwashes14pt7b.h"
#include "fonts/MeltSwashes16pt7b.h"
#include "fonts/Genty20pt7b.h"
#include <Arduino.h>

namespace ui_settings {

// ---------------------------------------------------------------------------
// Modal geometry (960x540 panel). The settings UI is now a centered modal
// window drawn over the existing framebuffer content; render() paints an
// opaque white rect so the full-width partial refresh of these rows shows
// the modal on top of whatever was underneath.
// ---------------------------------------------------------------------------
static constexpr int MODAL_X  = 100;
static constexpr int MODAL_Y  = 60;
static constexpr int MODAL_W  = 760;
static constexpr int MODAL_H  = 420;   // bottom = 480
static constexpr int MARGIN   = 10;    // inner padding
static constexpr int TITLE_H  = 56;
static constexpr int TAB_H    = 38;
static constexpr int ROW_H    = 46;
static constexpr int BOTTOM_H = 62;

static constexpr int MODAL_RIGHT  = MODAL_X + MODAL_W;
static constexpr int MODAL_BOTTOM = MODAL_Y + MODAL_H;

// Palette (4-bit grayscale, two nibbles per byte)
static constexpr uint8_t C_BLACK  = 0;
static constexpr uint8_t C_DKGRAY = 5;
static constexpr uint8_t C_LTGRAY = 12;
static constexpr uint8_t C_WHITE  = 15;

static constexpr uint8_t EPD_BLACK  = C_BLACK  << 4;
static constexpr uint8_t EPD_DKGRAY = C_DKGRAY << 4;
static constexpr uint8_t EPD_LTGRAY = C_LTGRAY << 4;
static constexpr uint8_t EPD_WHITE  = C_WHITE  << 4;

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------
enum Category { CAT_DISPLAY, CAT_POWER, CAT_COUNT };

enum SettingId {
  SET_NONE,
  SET_DAY_START,
  SET_DAY_END,
  SET_TIME_FORMAT,
  SET_REFRESH,
  SET_INACTIVITY,
  SET_SLEEP_START,
  SET_SLEEP_END,
  SET_RETENTION
};

struct RowDef {
  const char* label;
  SettingId id;
};

static const RowDef displayRows[] = {
  {"Day Start",     SET_DAY_START},
  {"Day End",       SET_DAY_END},
  {"Time Format",   SET_TIME_FORMAT},
};
static const RowDef powerRows[] = {
  {"Refresh Every", SET_REFRESH},
  {"Sleep After",   SET_INACTIVITY},
  {"Sleep Starts",  SET_SLEEP_START},
  {"Sleep Ends",    SET_SLEEP_END},
  {"Keep History",  SET_RETENTION},
};

struct CategoryDef {
  const char* name;
  const RowDef* rows;
  int rowCount;
};

static const CategoryDef categories[] = {
  {"Display", displayRows, 3},
  {"Power",   powerRows,   5},
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int  s_category    = CAT_DISPLAY;
static int  s_selectedRow = 0;

// Dirty tracking for partial refresh (y range within the modal).
static int  s_dirtyY1 = 0, s_dirtyY2 = 0;
static bool s_fullRedraw = false;

// ---------------------------------------------------------------------------
// Value cycling
// ---------------------------------------------------------------------------
static const int     startValues[]      = {5, 6, 7, 8, 9};
static const int     endValues[]        = {20, 21, 22, 23};
static const int     sleepStartValues[] = {20, 21, 22, 23};
static const int     sleepEndValues[]   = {5, 6, 7, 8, 9};
static const uint32_t refreshValues[]   = {1800, 3600, 7200, 14400, 21600};
static const uint32_t inactivityValues[] = {60, 120, 180, 300, 600};
static const uint32_t retentionValues[]  = {30, 90, 180, 365};

template<typename T>
static int findIndex(const T* arr, int count, T val) {
  for (int i = 0; i < count; i++)
    if (arr[i] == val) return i;
  return 0;
}

// Format a whole-hour setting value, respecting the 12h/24h preference.
static void formatHourSetting(char* buf, size_t len, int hour24, bool use24h) {
  if (use24h) {
    snprintf(buf, len, "%02d:00", hour24);
  } else {
    int h12 = hour24 % 12;
    if (h12 == 0) h12 = 12;
    const char* ap = (hour24 < 12) ? "AM" : "PM";
    snprintf(buf, len, "%d:00 %s", h12, ap);
  }
}

static void getValueStr(SettingId id, char* buf, size_t len) {
  const settings::Data& s = settings::get();
  switch (id) {
    case SET_DAY_START:   formatHourSetting(buf, len, s.day_start_hour, s.time_format_24h); break;
    case SET_DAY_END:     formatHourSetting(buf, len, s.day_end_hour, s.time_format_24h); break;
    case SET_TIME_FORMAT: snprintf(buf, len, "%s", s.time_format_24h ? "24 hour" : "12 hour"); break;
    case SET_SLEEP_START: formatHourSetting(buf, len, s.sleep_start_hour, s.time_format_24h); break;
    case SET_SLEEP_END:   formatHourSetting(buf, len, s.sleep_end_hour, s.time_format_24h); break;
    case SET_REFRESH:
      if (s.refresh_interval_s >= 3600)
        snprintf(buf, len, "%d hr", s.refresh_interval_s / 3600);
      else
        snprintf(buf, len, "%d min", s.refresh_interval_s / 60);
      break;
    case SET_INACTIVITY:
      if (s.inactivity_timeout_s >= 60)
        snprintf(buf, len, "%d min", s.inactivity_timeout_s / 60);
      else
        snprintf(buf, len, "%d sec", s.inactivity_timeout_s);
      break;
    case SET_RETENTION:
      if (s.history_retention_d >= 365) snprintf(buf, len, "1 year");
      else snprintf(buf, len, "%d days", s.history_retention_d);
      break;
    default: snprintf(buf, len, "?");
  }
}

static void cycleSetting(SettingId id, int delta) {
  settings::Data& s = settings::mutableRef();
  switch (id) {
    case SET_DAY_START: {
      int i = findIndex(startValues, 5, (int)s.day_start_hour);
      s.day_start_hour = startValues[((i + delta) % 5 + 5) % 5];
      break;
    }
    case SET_DAY_END: {
      int i = findIndex(endValues, 4, (int)s.day_end_hour);
      s.day_end_hour = endValues[((i + delta) % 4 + 4) % 4];
      break;
    }
    case SET_TIME_FORMAT: {
      s.time_format_24h = !s.time_format_24h;
      break;
    }
    case SET_SLEEP_START: {
      int i = findIndex(sleepStartValues, 4, (int)s.sleep_start_hour);
      s.sleep_start_hour = (uint8_t)sleepStartValues[((i + delta) % 4 + 4) % 4];
      break;
    }
    case SET_SLEEP_END: {
      int i = findIndex(sleepEndValues, 5, (int)s.sleep_end_hour);
      s.sleep_end_hour = (uint8_t)sleepEndValues[((i + delta) % 5 + 5) % 5];
      break;
    }
    case SET_REFRESH: {
      int n = 5;
      int i = findIndex(refreshValues, n, s.refresh_interval_s);
      s.refresh_interval_s = refreshValues[((i + delta) % n + n) % n];
      break;
    }
    case SET_INACTIVITY: {
      int n = 5;
      int i = findIndex(inactivityValues, n, s.inactivity_timeout_s);
      s.inactivity_timeout_s = inactivityValues[((i + delta) % n + n) % n];
      break;
    }
    case SET_RETENTION: {
      int n = 4;
      int i = findIndex(retentionValues, n, s.history_retention_d);
      s.history_retention_d = retentionValues[((i + delta) % n + n) % n];
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Layout helpers — every drawn rect and every hit-test rect is derived from
// these so the two can never drift.
// ---------------------------------------------------------------------------
static int contentTopY() { return MODAL_Y + TITLE_H + 4 + TAB_H + 8; }
static int rowY(int index) { return contentTopY() + index * ROW_H; }

static void rowRect(int rowIdx, int& rx, int& ry, int& rw, int& rh) {
  rx = MODAL_X + MARGIN;
  ry = rowY(rowIdx);
  rw = MODAL_W - 2 * MARGIN;
  rh = ROW_H - 4;
}

// Two equal tabs split across the inner width with an 8px gap.
static void tabRects(int& tab0x, int& tab1x, int& tabY, int& tabW, int& tabH) {
  int innerX = MODAL_X + MARGIN;
  int innerW = MODAL_W - 2 * MARGIN;
  tabW = (innerW - 8) / 2;
  tab0x = innerX;
  tab1x = innerX + tabW + 8;
  tabY = MODAL_Y + TITLE_H + 4;
  tabH = TAB_H;
}

static void closeBtnRect(int& cx, int& cy, int& cw, int& ch) {
  cw = 36;
  ch = 36;
  cx = MODAL_RIGHT - cw - 10;
  cy = MODAL_Y + (TITLE_H - ch) / 2;   // vertically centered in the title bar
}

static void bottomBtnRects(int& minusX, int& plusX, int& saveX, int& saveW, int& btnY, int& btnSize) {
  btnSize = 48;
  btnY = MODAL_BOTTOM - BOTTOM_H + (BOTTOM_H - btnSize) / 2;
  minusX = MODAL_X + MARGIN + 40;
  plusX  = minusX + btnSize + 24;
  saveW  = 120;
  saveX  = MODAL_RIGHT - MARGIN - saveW;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// Small white battery glyph for the black title bar (~31x14 incl. nub).
static void drawBatteryTiny(int x, int y, int percent, uint8_t* fb) {
  epd_draw_rect(x, y, 28, 14, EPD_WHITE, fb);
  epd_fill_rect(x + 28, y + 4, 3, 6, EPD_WHITE, fb);
  int p = percent;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  int fillW = (p * 24) / 100;
  if (fillW > 0) {
    epd_fill_rect(x + 2, y + 2, fillW, 10, EPD_WHITE, fb);
  }
}

static void drawSettingRow(int rowIdx, bool selected, uint8_t* fb) {
  const RowDef* rows = categories[s_category].rows;
  int rx, ry, rw, rh;
  rowRect(rowIdx, rx, ry, rw, rh);

  // Always clear the row to white first, then apply the highlight.
  epd_fill_rect(rx, ry, rw, rh, EPD_WHITE, fb);
  if (selected) {
    epd_fill_rect(rx, ry, rw, rh, EPD_LTGRAY, fb);
  }

  // Label (left)
  {
    int32_t lx = rx + 16, ly = ry + ROW_H / 2 + 6;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = selected ? C_LTGRAY : C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, rows[rowIdx].label, &lx, &ly, fb, BLACK_ON_WHITE, &props);
  }

  // Value (right-aligned)
  char valBuf[32];
  getValueStr(rows[rowIdx].id, valBuf, sizeof(valBuf));
  {
    int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
    int32_t tcx = 0, tcy = 0;
    get_text_bounds((GFXfont*)&MeltSwashes16, valBuf, &tcx, &tcy, &tx1, &ty1, &tw, &th, NULL);
    int32_t vx = rx + rw - tw - 16, vy = ry + ROW_H / 2 + 6;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = selected ? C_LTGRAY : C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, valBuf, &vx, &vy, fb, BLACK_ON_WHITE, &props);
  }

  // Selection indicator on the left
  if (selected) {
    int32_t ax = rx + 2, ay = ry + ROW_H / 2 + 6;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = C_LTGRAY;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, ">", &ax, &ay, fb, BLACK_ON_WHITE, &props);
  }
}

static void drawTabs(uint8_t* fb) {
  int tab0x, tab1x, tabY, tabW, tabH;
  tabRects(tab0x, tab1x, tabY, tabW, tabH);
  for (int c = 0; c < CAT_COUNT; c++) {
    int tx = (c == 0) ? tab0x : tab1x;
    bool active = (c == s_category);
    const char* name = categories[c].name;

    if (active) {
      epd_fill_rect(tx, tabY, tabW, tabH, EPD_DKGRAY, fb);
    } else {
      epd_fill_rect(tx, tabY, tabW, tabH, EPD_WHITE, fb);
      epd_draw_rect(tx, tabY, tabW, tabH, EPD_BLACK, fb);
    }

    // Center the tab label.
    int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
    int32_t mcx = tx, mcy = tabY + tabH / 2 + 6;
    get_text_bounds((GFXfont*)&MeltSwashes16, name, &mcx, &mcy, &tx1, &ty1, &tw, &th, NULL);
    int32_t lx = tx + (tabW - tw) / 2, ly = tabY + tabH / 2 + 6;
    FontProperties props;
    props.fg_color = active ? C_WHITE : C_BLACK;
    props.bg_color = active ? C_DKGRAY : C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, name, &lx, &ly, fb, BLACK_ON_WHITE, &props);
  }
}

static void drawBottomBar(uint8_t* fb) {
  int minusX, plusX, saveX, saveW, btnY, btnSize;
  bottomBtnRects(minusX, plusX, saveX, saveW, btnY, btnSize);

  // Separator line at the top of the bottom region.
  epd_draw_hline(MODAL_X + MARGIN, MODAL_BOTTOM - BOTTOM_H,
                 MODAL_W - 2 * MARGIN, EPD_LTGRAY, fb);

  // − button (left-pointing filled triangle)
  epd_fill_triangle(minusX, btnY + btnSize / 2,
                    minusX + btnSize, btnY,
                    minusX + btnSize, btnY + btnSize,
                    EPD_DKGRAY, fb);
  epd_draw_line(minusX, btnY + btnSize / 2, minusX + btnSize, btnY, EPD_BLACK, fb);
  epd_draw_line(minusX + btnSize, btnY, minusX + btnSize, btnY + btnSize, EPD_BLACK, fb);
  epd_draw_line(minusX + btnSize, btnY + btnSize, minusX, btnY + btnSize / 2, EPD_BLACK, fb);

  // + button (right-pointing filled triangle)
  epd_fill_triangle(plusX + btnSize, btnY + btnSize / 2,
                    plusX, btnY,
                    plusX, btnY + btnSize,
                    EPD_DKGRAY, fb);
  epd_draw_line(plusX + btnSize, btnY + btnSize / 2, plusX, btnY, EPD_BLACK, fb);
  epd_draw_line(plusX, btnY, plusX, btnY + btnSize, EPD_BLACK, fb);
  epd_draw_line(plusX, btnY + btnSize, plusX + btnSize, btnY + btnSize / 2, EPD_BLACK, fb);

  // Save button (filled DKGRAY, white text)
  epd_fill_rect(saveX, btnY, saveW, btnSize, EPD_DKGRAY, fb);
  epd_draw_rect(saveX, btnY, saveW, btnSize, EPD_BLACK, fb);
  {
    const char* txt = "Save";
    int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
    int32_t mcx = saveX, mcy = btnY + btnSize / 2 + 6;
    get_text_bounds((GFXfont*)&MeltSwashes16, txt, &mcx, &mcy, &tx1, &ty1, &tw, &th, NULL);
    int32_t lx = saveX + (saveW - tw) / 2, ly = btnY + btnSize / 2 + 6;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_DKGRAY;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, txt, &lx, &ly, fb, BLACK_ON_WHITE, &props);
  }
}

// ---------------------------------------------------------------------------
// Full modal render
// ---------------------------------------------------------------------------
void render() {
  uint8_t* fb = display_mgr::framebuffer();

  // Opaque modal background. Do NOT memset the framebuffer — the surrounding
  // full-width rows must keep showing the underlying view.
  epd_fill_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, EPD_WHITE, fb);

  // Modal border
  epd_draw_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, EPD_BLACK, fb);

  // Title bar (full modal width, inverse video)
  epd_fill_rect(MODAL_X, MODAL_Y, MODAL_W, TITLE_H, EPD_BLACK, fb);

  // Title text "Settings"
  {
    int32_t cx = MODAL_X + 16, cy = MODAL_Y + TITLE_H / 2 + 8;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_BLACK;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&Genty20, "Settings", &cx, &cy, fb, BLACK_ON_WHITE, &props);
  }

  // Battery readout to the right of the title.
  {
    int32_t titleW = 0, titleH = 0, tx1 = 0, ty1 = 0;
    int32_t mcx = MODAL_X + 16, mcy = MODAL_Y + TITLE_H / 2 + 8;
    get_text_bounds((GFXfont*)&Genty20, "Settings", &mcx, &mcy, &tx1, &ty1, &titleW, &titleH, NULL);

    int iconX = (MODAL_X + 16) + titleW + 16;
    int iconY = MODAL_Y + (TITLE_H - 14) / 2;
    int percent = battery::lastPercent();
    drawBatteryTiny(iconX, iconY, percent, fb);

    char buf[8];
    if (percent < 0) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%d%%", percent);
    int32_t ptx = iconX + 28 + 3 + 6, pty = MODAL_Y + TITLE_H / 2 + 8;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_BLACK;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes14, buf, &ptx, &pty, fb, BLACK_ON_WHITE, &props);
  }

  // Close button: white square with a black X (high contrast on the black title bar)
  {
    int cxb, cyb, cwb, chb;
    closeBtnRect(cxb, cyb, cwb, chb);
    epd_fill_rect(cxb, cyb, cwb, chb, EPD_WHITE, fb);
    int inset = 9;
    epd_draw_line(cxb + inset, cyb + inset, cxb + cwb - inset, cyb + chb - inset, EPD_BLACK, fb);
    epd_draw_line(cxb + cwb - inset, cyb + inset, cxb + inset, cyb + chb - inset, EPD_BLACK, fb);
  }

  // Tabs
  drawTabs(fb);

  // Content rows
  int rc = categories[s_category].rowCount;
  for (int i = 0; i < rc; i++) {
    drawSettingRow(i, i == s_selectedRow, fb);
  }

  // Bottom bar
  drawBottomBar(fb);
}

void markFullRedraw() { s_fullRedraw = true; }

void getDirtyRect(int& x, int& y, int& w, int& h) {
  if (s_fullRedraw) {
    x = MODAL_X;
    y = MODAL_Y;
    w = MODAL_W;
    h = MODAL_H;
    return;
  }
  x = MODAL_X;
  y = s_dirtyY1;
  w = MODAL_W;
  h = s_dirtyY2 - s_dirtyY1;
  if (h < 1) h = ROW_H;
}

// ---------------------------------------------------------------------------
// Tap handling
// ---------------------------------------------------------------------------
TapResult handleTap(int16_t x, int16_t y) {
  int rc = categories[s_category].rowCount;

  // Close button
  {
    int cxb, cyb, cwb, chb;
    closeBtnRect(cxb, cyb, cwb, chb);
    if (x >= cxb && x <= cxb + cwb && y >= cyb && y <= cyb + chb) {
      return TAP_CLOSE;
    }
  }

  // Tabs
  {
    int tab0x, tab1x, tabY, tabW, tabH;
    tabRects(tab0x, tab1x, tabY, tabW, tabH);
    for (int c = 0; c < CAT_COUNT; c++) {
      int tx = (c == 0) ? tab0x : tab1x;
      if (x >= tx && x <= tx + tabW && y >= tabY && y <= tabY + tabH) {
        if (c != s_category) {
          s_category = c;
          s_selectedRow = 0;
          s_fullRedraw = true;
          return TAP_FULL;
        }
        return TAP_NONE;
      }
    }
  }

  // Content rows — tap to select
  for (int i = 0; i < rc; i++) {
    int rx, ry, rw, rh;
    rowRect(i, rx, ry, rw, rh);
    if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh) {
      if (i != s_selectedRow) {
        uint8_t* fb = display_mgr::framebuffer();
        int oldRow = s_selectedRow;
        drawSettingRow(oldRow, false, fb);
        s_selectedRow = i;
        drawSettingRow(s_selectedRow, true, fb);
        int lo = (oldRow < i) ? oldRow : i;
        int hi = (oldRow > i) ? oldRow : i;
        s_dirtyY1 = rowY(lo);
        s_dirtyY2 = rowY(hi) + ROW_H;
        s_fullRedraw = false;
        return TAP_PARTIAL;
      }
      return TAP_NONE;
    }
  }

  // Bottom bar buttons
  int minusX, plusX, saveX, saveW, btnY, btnSize;
  bottomBtnRects(minusX, plusX, saveX, saveW, btnY, btnSize);

  // Save
  if (x >= saveX && x <= saveX + saveW && y >= btnY && y <= btnY + btnSize) {
    settings::save();
    Serial.println("[settings] Saved to SD card");
    return TAP_CLOSE;
  }

  // − button
  if (x >= minusX && x <= minusX + btnSize && y >= btnY && y <= btnY + btnSize) {
    const RowDef* rows = categories[s_category].rows;
    cycleSetting(rows[s_selectedRow].id, -1);
    uint8_t* fb = display_mgr::framebuffer();
    drawSettingRow(s_selectedRow, true, fb);
    s_dirtyY1 = rowY(s_selectedRow);
    s_dirtyY2 = s_dirtyY1 + ROW_H;
    s_fullRedraw = false;
    return TAP_PARTIAL;
  }

  // + button
  if (x >= plusX && x <= plusX + btnSize && y >= btnY && y <= btnY + btnSize) {
    const RowDef* rows = categories[s_category].rows;
    cycleSetting(rows[s_selectedRow].id, +1);
    uint8_t* fb = display_mgr::framebuffer();
    drawSettingRow(s_selectedRow, true, fb);
    s_dirtyY1 = rowY(s_selectedRow);
    s_dirtyY2 = s_dirtyY1 + ROW_H;
    s_fullRedraw = false;
    return TAP_PARTIAL;
  }

  return TAP_NONE;
}

} // namespace ui_settings
