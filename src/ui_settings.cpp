#include "ui_settings.h"
#include "settings.h"
#include "display_manager.h"
#include "epd_driver.h"
#include "fonts/MeltSwashes14pt7b.h"
#include "fonts/MeltSwashes16pt7b.h"
#include "fonts/Genty20pt7b.h"
#include <Arduino.h>
#include <string.h>

namespace ui_settings {

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr int MARGIN      = 8;
static constexpr int SIDEBAR_W   = 120;
static constexpr int TITLE_H     = 44;
static constexpr int ROW_H       = 48;
static constexpr int BOTTOM_Y    = 478;
static constexpr int BOTTOM_H    = 54;
static constexpr int CONTENT_X   = SIDEBAR_W + MARGIN + 4;
static constexpr int CONTENT_W   = EPD_WIDTH - CONTENT_X - MARGIN;

// Palette
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
enum Category { CAT_DISPLAY, CAT_POWER, CAT_NETWORK, CAT_COUNT };

// Setting IDs
enum SettingId {
  SET_NONE,
  SET_DAY_START,
  SET_DAY_END,
  SET_REFRESH,
  SET_INACTIVITY,
  SET_RETENTION,
  SET_SSID,
  SET_MQTT_HOST,
  SET_MQTT_TOPIC
};

struct RowDef {
  const char* label;
  SettingId id;
};

// Rows per category
static const RowDef displayRows[] = {
  {"Day Start",    SET_DAY_START},
  {"Day End",      SET_DAY_END},
};
static const RowDef powerRows[] = {
  {"Refresh Every",  SET_REFRESH},
  {"Sleep After",    SET_INACTIVITY},
  {"Keep History",   SET_RETENTION},
};
static const RowDef networkRows[] = {
  {"WiFi SSID",    SET_SSID},
  {"MQTT Host",    SET_MQTT_HOST},
  {"MQTT Topic",   SET_MQTT_TOPIC},
};

struct CategoryDef {
  const char* name;
  const RowDef* rows;
  int rowCount;
  const char* icon;
};

static const CategoryDef categories[] = {
  {"Display", displayRows, 2, "D"},
  {"Power",   powerRows,   3, "P"},
  {"Network", networkRows, 3, "N"},
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int s_category = CAT_DISPLAY;
static int s_selectedRow = 0;
static bool s_entered = false;  // first render after entering

// Dirty tracking for partial refresh
static int s_dirtyY1 = 0, s_dirtyY2 = 0;  // y range to refresh

// ---------------------------------------------------------------------------
// Value cycling
// ---------------------------------------------------------------------------
static const int startValues[] = {5, 6, 7, 8, 9};
static const int endValues[] = {20, 21, 22, 23};
static const uint32_t refreshValues[] = {1800, 3600, 7200, 14400, 21600};
static const uint32_t inactivityValues[] = {15, 30, 45, 60, 120, 300};
static const uint32_t retentionValues[] = {30, 90, 180, 365};

template<typename T>
static int findIndex(const T* arr, int count, T val) {
  for (int i = 0; i < count; i++)
    if (arr[i] == val) return i;
  return 0;
}

static void getValueStr(SettingId id, char* buf, size_t len) {
  const settings::Data& s = settings::get();
  switch (id) {
    case SET_DAY_START:   snprintf(buf, len, "%d:00", s.day_start_hour); break;
    case SET_DAY_END:     snprintf(buf, len, "%d:00", s.day_end_hour); break;
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
    case SET_SSID:       snprintf(buf, len, "%.24s", s.wifi_ssid); break;
    case SET_MQTT_HOST:  snprintf(buf, len, "%.24s", s.mqtt_host); break;
    case SET_MQTT_TOPIC: snprintf(buf, len, "%.24s", s.mqtt_topic); break;
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
    case SET_REFRESH: {
      int n = 5;
      int i = findIndex(refreshValues, n, s.refresh_interval_s);
      s.refresh_interval_s = refreshValues[((i + delta) % n + n) % n];
      break;
    }
    case SET_INACTIVITY: {
      int n = 6;
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
// Layout helpers
// ---------------------------------------------------------------------------
static int contentTopY() { return MARGIN + TITLE_H + 8; }

static int rowY(int index) {
  return contentTopY() + index * ROW_H;
}

static int sidebarBtnY(int cat) {
  int sidebarStart = contentTopY();
  int sidebarBtnH = 80;
  int gap = 12;
  return sidebarStart + cat * (sidebarBtnH + gap);
}

static void bottomBtnRects(int& backX, int& minusX, int& plusX, int& saveX, int& btnY) {
  int btnSize = 60;
  btnY = BOTTOM_Y + (BOTTOM_H - btnSize) / 2;
  backX  = CONTENT_X;
  minusX = CONTENT_X + 180;
  plusX  = CONTENT_X + 260;
  saveX  = EPD_WIDTH - MARGIN - 120;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void drawSidebarButton(int cat, bool active, uint8_t* fb) {
  int y = sidebarBtnY(cat);
  int x = MARGIN;
  int w = SIDEBAR_W - MARGIN - 4;
  int h = 80;

  if (active) {
    epd_fill_rect(x, y, w, h, EPD_DKGRAY, fb);
    epd_draw_rect(x, y, w, h, EPD_BLACK, fb);
  } else {
    epd_draw_rect(x, y, w, h, EPD_BLACK, fb);
  }

  // Icon placeholder — a single letter in a circle
  int iconCx = x + w / 2;
  int iconCy = y + 26;
  if (active) {
    epd_draw_circle(iconCx, iconCy, 16, EPD_WHITE, fb);
  } else {
    epd_fill_circle(iconCx, iconCy, 16, EPD_LTGRAY, fb);
    epd_draw_circle(iconCx, iconCy, 16, EPD_BLACK, fb);
  }

  // Category initial inside circle
  {
    int32_t cx = iconCx - 6, cy = iconCy + 6;
    FontProperties props;
    props.fg_color = active ? C_WHITE : C_BLACK;
    props.bg_color = active ? C_DKGRAY : C_LTGRAY;
    props.flags = 0;
    props.fallback_glyph = 0;
    char letter[2] = {categories[cat].icon[0], 0};
    write_mode((GFXfont*)&MeltSwashes16, letter, &cx, &cy, fb, BLACK_ON_WHITE, &props);
  }

  // Category name below icon
  {
    int32_t lx = x + 4, ly = y + h - 12;
    FontProperties props;
    props.fg_color = active ? C_WHITE : C_BLACK;
    props.bg_color = active ? C_DKGRAY : C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes14, categories[cat].name, &lx, &ly, fb, BLACK_ON_WHITE, &props);
  }
}

static void drawSettingRow(int rowIdx, bool selected, uint8_t* fb) {
  const RowDef* rows = categories[s_category].rows;
  int y = rowY(rowIdx);
  int x = CONTENT_X;
  int w = CONTENT_W;

  // ALWAYS clear the row to white first — removes any previous
  // selection highlight before drawing the new state.
  epd_fill_rect(x, y, w, ROW_H - 4, EPD_WHITE, fb);

  if (selected) {
    // Then apply the gray highlight for the selected row
    epd_fill_rect(x, y, w, ROW_H - 4, EPD_LTGRAY, fb);
  }

  // Label (left)
  {
    int32_t lx = x + 16, ly = y + ROW_H / 2 + 6;
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
    int32_t vx = x + w - tw - 16, vy = y + ROW_H / 2 + 6;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = selected ? C_LTGRAY : C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, valBuf, &vx, &vy, fb, BLACK_ON_WHITE, &props);
  }

  // Selection indicator (▸) on left
  if (selected) {
    int32_t ax = x + 2, ay = y + ROW_H / 2 + 6;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = C_LTGRAY;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, ">", &ax, &ay, fb, BLACK_ON_WHITE, &props);
  }
}

static void drawBottomBar(uint8_t* fb) {
  int backX, minusX, plusX, saveX, btnY;
  bottomBtnRects(backX, minusX, plusX, saveX, btnY);

  // Separator line
  epd_draw_hline(MARGIN, BOTTOM_Y, EPD_WIDTH - MARGIN * 2, EPD_LTGRAY, fb);

  int btnSize = 60;

  // Back button
  epd_draw_rect(backX, btnY, 120, btnSize, EPD_BLACK, fb);
  {
    int32_t cx = backX + 30, cy = btnY + btnSize / 2 + 6;
    writeln((GFXfont*)&MeltSwashes16, "Back", &cx, &cy, fb);
  }

  // − button (filled triangle, left-pointing)
  epd_fill_triangle(minusX, btnY + btnSize/2,
                    minusX + btnSize, btnY,
                    minusX + btnSize, btnY + btnSize,
                    EPD_DKGRAY, fb);
  epd_draw_line(minusX, btnY + btnSize/2, minusX + btnSize, btnY, EPD_BLACK, fb);
  epd_draw_line(minusX + btnSize, btnY, minusX + btnSize, btnY + btnSize, EPD_BLACK, fb);
  epd_draw_line(minusX + btnSize, btnY + btnSize, minusX, btnY + btnSize/2, EPD_BLACK, fb);

  // + button (filled triangle, right-pointing)
  epd_fill_triangle(plusX + btnSize, btnY + btnSize/2,
                    plusX, btnY,
                    plusX, btnY + btnSize,
                    EPD_DKGRAY, fb);
  epd_draw_line(plusX + btnSize, btnY + btnSize/2, plusX, btnY, EPD_BLACK, fb);
  epd_draw_line(plusX, btnY, plusX, btnY + btnSize, EPD_BLACK, fb);
  epd_draw_line(plusX, btnY + btnSize, plusX + btnSize, btnY + btnSize/2, EPD_BLACK, fb);

  // Save button
  epd_fill_rect(saveX, btnY, 120, btnSize, EPD_DKGRAY, fb);
  epd_draw_rect(saveX, btnY, 120, btnSize, EPD_BLACK, fb);
  {
    int32_t cx = saveX + 28, cy = btnY + btnSize / 2 + 6;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_DKGRAY;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, "Save", &cx, &cy, fb, BLACK_ON_WHITE, &props);
  }
}

// ---------------------------------------------------------------------------
// Full render
// ---------------------------------------------------------------------------
void render() {
  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  // Outer border
  epd_draw_rect(MARGIN, MARGIN, EPD_WIDTH - MARGIN*2, EPD_HEIGHT - MARGIN*2, EPD_BLACK, fb);

  // Title bar (full width, inverse video)
  int titleY = MARGIN + 2;
  epd_fill_rect(MARGIN + 1, titleY, EPD_WIDTH - MARGIN*2 - 2, TITLE_H - 4, EPD_BLACK, fb);
  {
    int32_t cx = MARGIN + 16, cy = titleY + 30;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_BLACK;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&Genty20, "Settings", &cx, &cy, fb, BLACK_ON_WHITE, &props);
  }

  // Debug badge — placed to the right of the "Settings" title with a small
  // gap. We measure the title width first so the badge never overlaps it.
  {
    int32_t titleW = 0, titleH = 0, tx1 = 0, ty1 = 0;
    int32_t measureX = MARGIN + 16, measureY = titleY + 30;
    get_text_bounds((GFXfont*)&Genty20, "Settings", &measureX, &measureY,
                    &tx1, &ty1, &titleW, &titleH, NULL);
    int32_t tx = (MARGIN + 16) + titleW + 12;
    int32_t ty = titleY + 30;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_BLACK;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes14, "[DEBUG]", &tx, &ty, fb, BLACK_ON_WHITE, &props);
  }

  // Close button
  int closeX = EPD_WIDTH - MARGIN - 60;
  int closeY = titleY + 4;
  epd_draw_rect(closeX, closeY, 48, 32, EPD_WHITE, fb);
  {
    int32_t cx = closeX + 8, cy = closeY + 24;
    FontProperties props;
    props.fg_color = C_WHITE;
    props.bg_color = C_BLACK;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, "Close", &cx, &cy, fb, BLACK_ON_WHITE, &props);
  }

  // Sidebar separator
  int sidebarRight = SIDEBAR_W + 2;
  epd_draw_vline(sidebarRight, contentTopY() - 4, BOTTOM_Y - contentTopY() + 4, EPD_LTGRAY, fb);

  // Sidebar buttons
  for (int c = 0; c < CAT_COUNT; c++) {
    drawSidebarButton(c, c == s_category, fb);
  }

  // Content rows
  int rc = categories[s_category].rowCount;
  for (int i = 0; i < rc; i++) {
    drawSettingRow(i, i == s_selectedRow, fb);
  }

  // Bottom bar
  drawBottomBar(fb);

  s_entered = true;
}

// ---------------------------------------------------------------------------
// Dirty row redraw (for partial refresh)
// ---------------------------------------------------------------------------
void redrawDirty() {
  // redrawDirty is called after row state changes.
  // The dirty rows have already been redrawn by handleTap via drawSettingRow.
  // This function is a no-op — the framebuffer is already updated.
  // The caller uses getDirtyRect() to know what to partial-refresh.
}

void getDirtyRect(int& x, int& y, int& w, int& h) {
  x = CONTENT_X;
  y = s_dirtyY1;
  w = CONTENT_W;
  h = s_dirtyY2 - s_dirtyY1;
  if (h < 1) h = ROW_H;
}

// ---------------------------------------------------------------------------
// Tap handling
// ---------------------------------------------------------------------------
TapResult handleTap(int16_t x, int16_t y) {
  int rc = categories[s_category].rowCount;

  // Close button
  int closeX = EPD_WIDTH - MARGIN - 60;
  int closeY = MARGIN + 2 + 4;
  if (x >= closeX && x <= closeX + 48 && y >= closeY && y <= closeY + 32) {
    return TAP_CLOSE;
  }

  // Sidebar category buttons
  for (int c = 0; c < CAT_COUNT; c++) {
    int by = sidebarBtnY(c);
    int bx = MARGIN;
    int bw = SIDEBAR_W - MARGIN - 4;
    int bh = 80;
    if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
      if (c != s_category) {
        s_category = c;
        s_selectedRow = 0;
        return TAP_FULL;
      }
      return TAP_NONE;
    }
  }

  // Content rows — tap to select
  for (int i = 0; i < rc; i++) {
    int ry = rowY(i);
    if (x >= CONTENT_X && x <= CONTENT_X + CONTENT_W &&
        y >= ry && y <= ry + ROW_H - 4) {
      if (i != s_selectedRow) {
        // Redraw old row (un-selected) and new row (selected)
        uint8_t* fb = display_mgr::framebuffer();
        int oldRow = s_selectedRow;
        drawSettingRow(oldRow, false, fb);
        s_selectedRow = i;
        drawSettingRow(s_selectedRow, true, fb);
        int lo = (oldRow < i) ? oldRow : i;
        int hi = (oldRow > i) ? oldRow : i;
        s_dirtyY1 = rowY(lo);
        s_dirtyY2 = rowY(hi) + ROW_H;
        return TAP_PARTIAL;
      }
      return TAP_NONE;
    }
  }

  // Bottom bar buttons
  int backX, minusX, plusX, saveX, btnY;
  bottomBtnRects(backX, minusX, plusX, saveX, btnY);
  int btnSize = 60;

  // Back
  if (x >= backX && x <= backX + 120 && y >= btnY && y <= btnY + btnSize) {
    return TAP_CLOSE;
  }

  // Save
  if (x >= saveX && x <= saveX + 120 && y >= btnY && y <= btnY + btnSize) {
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
    return TAP_PARTIAL;
  }

  return TAP_NONE;
}

} // namespace ui_settings
