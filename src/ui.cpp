#include "ui.h"
#include "dashboards/calendar_dashboard.h"
#include "display_manager.h"
#include "battery.h"
#include "epd_driver.h"
#include "fonts/Genty16pt7b.h"
#include "fonts/Genty20pt7b.h"
#include "fonts/Genty24pt7b.h"
#include "fonts/Genty32pt7b.h"
#include "fonts/Genty48pt7b.h"
#include "fonts/Melt-Swashes14pt7b.h"
#include "fonts/Melt-Swashes16pt7b.h"
#include <Arduino.h>
#include <time.h>
#include <string.h>
#include <cstdio>

namespace ui {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static constexpr uint8_t C_BLACK  = 0;
static constexpr uint8_t C_DKGRAY = 5;
static constexpr uint8_t C_MDGRAY = 8;
static constexpr uint8_t C_LTGRAY = 12;
static constexpr uint8_t C_WHITE  = 15;

static constexpr uint8_t EPD_BLACK  = C_BLACK  << 4;
static constexpr uint8_t EPD_DKGRAY = C_DKGRAY << 4;
static constexpr uint8_t EPD_LTGRAY = C_LTGRAY << 4;
static constexpr uint8_t EPD_WHITE  = C_WHITE  << 4;

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr int WIN_MARGIN     = 12;
static constexpr int HEADER_H       = 90;
static constexpr int DAY_START_HOUR = 7;
static constexpr int DAY_END_HOUR   = 22;
static constexpr int NUM_COLS       = 4;

// Daily view layout — shared between renderDailyView() and hitBackButton()
// to prevent coordinate mismatches.
static constexpr int DAILY_PAGE_SIZE    = 280;  // tear-off calendar square
static constexpr int DAILY_PAGE_MARGIN  = 16;   // offset from content rect
static constexpr int DAILY_BACK_W       = 120;  // back button width
static constexpr int DAILY_BACK_H       = 48;   // back button height
static constexpr int DAILY_BACK_GAP     = 12;   // gap between calendar and back button
static constexpr int DAILY_LIST_GAP     = 24;   // gap between calendar column and list
static constexpr int DAILY_ROW_H        = 96;   // event list row height (per ui.txt)
static constexpr int DAILY_STATUS_H     = 32;   // status strip height

// ---------------------------------------------------------------------------
// External data source
// ---------------------------------------------------------------------------
static const CalendarEvent* s_events = nullptr;
static int s_eventCount = 0;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Screen s_screen = SCREEN_WEEKLY;
static int        s_baseDayOffset = 0;

static bool       s_pendingRender = false;

// Touch state machine
enum TouchPhase { TOUCH_IDLE, TOUCH_DOWN };
static TouchPhase s_phase = TOUCH_IDLE;
static int16_t    s_startX = 0;
static int16_t    s_startY = 0;
static int16_t    s_lastX  = 0;
static int16_t    s_lastY  = 0;
static unsigned long s_startMs = 0;
static unsigned long s_cooldownUntilMs = 0;

static constexpr unsigned long TAP_MAX_MS         = 500;
static constexpr unsigned long SWIPE_MAX_MS       = 600;
static constexpr int           SWIPE_MIN_PX       = 40;
static constexpr unsigned long GESTURE_COOLDOWN_MS = 300;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const char* dayName(int wday) {
  static const char* names[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday" };
  return names[wday % 7];
}

static const char* dayNameShort(int wday) {
  static const char* names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  return names[wday % 7];
}

static const char* monthName(int mon) {
  static const char* names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  return names[mon % 12];
}

static void getDemoBaseDay(struct tm& out) {
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  out.tm_hour = 12; out.tm_min = 0; out.tm_sec = 0;
  mktime(&out);
}

// Format a struct tm as "YYYY-MM-DD"
static void dateToString(const struct tm& day, char* out, size_t len) {
  snprintf(out, len, "%04d-%02d-%02d",
           day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
}

// Find all events matching the given date string. Returns count, fills outIndices.
static int eventsForDate(const char* dateStr, int* outIndices, int maxIndices) {
  int n = 0;
  for (int i = 0; i < s_eventCount && n < maxIndices; i++) {
    if (strcmp(s_events[i].date, dateStr) == 0) {
      outIndices[n++] = i;
    }
  }
  return n;
}

static void formatTime(int hour, int min, char* buf, size_t len) {
  int h = hour % 12;
  if (h == 0) h = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  snprintf(buf, len, "%d:%02d %s", h, min, ampm);
}

static void truncateWithEllipsis(const char* str, char* out, size_t outLen, int maxChars) {
  int len = strlen(str);
  if (len <= maxChars) {
    strlcpy(out, str, outLen);
  } else {
    int keep = maxChars - 3;
    if (keep < 1) keep = 1;
    strlcpy(out, str, keep + 1);
    strlcat(out, "...", outLen);
  }
}

static void drawCenteredText(const char* str, int cx, int cy, GFXfont* font, uint8_t* fb) {
  int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
  int32_t tcx = cx, tcy = cy;
  get_text_bounds(font, str, &tcx, &tcy, &tx1, &ty1, &tw, &th, NULL);
  tcx = cx - tw / 2;
  tcy = cy;
  writeln(font, str, &tcx, &tcy, fb);
}

static void drawCenteredTextColored(const char* str, int cx, int cy, GFXfont* font,
                                    uint8_t* fb, uint8_t fg, uint8_t bg) {
  int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
  int32_t tcx = cx, tcy = cy;
  get_text_bounds(font, str, &tcx, &tcy, &tx1, &ty1, &tw, &th, NULL);
  tcx = cx - tw / 2;
  tcy = cy;
  FontProperties props;
  props.fg_color = fg;
  props.bg_color = bg;
  props.flags = 0;
  props.fallback_glyph = 0;
  write_mode(font, str, &tcx, &tcy, fb, BLACK_ON_WHITE, &props);
}

// Draw a colored string at (x, y) using write_mode with the given fg/bg.
// The LilyGo renderer handles character spacing and kerning correctly when
// the entire string is drawn in a single call.
static void drawTextColored(GFXfont* font, const char* str, int x, int y,
                            uint8_t* fb, uint8_t fg, uint8_t bg) {
  FontProperties props;
  props.fg_color = fg;
  props.bg_color = bg;
  props.flags = 0;
  props.fallback_glyph = 0;
  int32_t cx = x, cy = y;
  write_mode(font, str, &cx, &cy, fb, BLACK_ON_WHITE, &props);
}

// ---------------------------------------------------------------------------
// UI chrome
// ---------------------------------------------------------------------------
static void drawBatteryIcon(int x, int y, int w, int h, int percent, uint8_t* fb) {
  epd_fill_rect(x, y, w, h, EPD_WHITE, fb);
  epd_draw_rect(x, y, w, h, EPD_BLACK, fb);
  epd_fill_rect(x + w, y + h / 4, 4, h / 2, EPD_BLACK, fb);

  int segW = (w - 6) / 4;
  int activeSegs = (percent <= 0)  ? 0 :
                   (percent <= 25) ? 1 :
                   (percent <= 50) ? 2 :
                   (percent <= 75) ? 3 : 4;
  for (int i = 0; i < 4; i++) {
    int sx = x + 3 + i * segW;
    if (i < activeSegs) {
      epd_fill_rect(sx + 1, y + 3, segW - 2, h - 6, EPD_BLACK, fb);
    } else {
      epd_draw_rect(sx + 1, y + 3, segW - 2, h - 6, EPD_BLACK, fb);
    }
  }
}

static void drawArrowButton(int x, int y, int w, int h, bool left, uint8_t* fb) {
  epd_fill_rect(x, y, w, h, EPD_WHITE, fb);
  epd_draw_rect(x, y, w, h, EPD_BLACK, fb);

  int cx = x + w / 2;
  int cy = y + h / 2;
  int len = h / 5;
  if (left) {
    epd_draw_line(cx + len/2, cy - len, cx - len/2, cy, EPD_BLACK, fb);
    epd_draw_line(cx - len/2, cy, cx + len/2, cy + len, EPD_BLACK, fb);
  } else {
    epd_draw_line(cx - len/2, cy - len, cx + len/2, cy, EPD_BLACK, fb);
    epd_draw_line(cx + len/2, cy, cx - len/2, cy + len, EPD_BLACK, fb);
  }
}

static void drawTearOffCalendar(int x, int y, int size, const struct tm& day, uint8_t* fb) {
  int bindingH = 34;
  int tearH = 14;

  epd_fill_rect(x, y, size, size, EPD_WHITE, fb);
  epd_draw_rect(x, y, size, size, EPD_BLACK, fb);

  // Binding strip at top
  epd_fill_rect(x, y, size, bindingH, EPD_DKGRAY, fb);
  epd_draw_rect(x, y, size, bindingH, EPD_BLACK, fb);

  // Rings
  for (int rx = x + 22; rx < x + size; rx += 44) {
    epd_fill_rect(rx, y + 10, 16, 16, EPD_WHITE, fb);
    epd_draw_rect(rx, y + 10, 16, 16, EPD_BLACK, fb);
  }

  // Zigzag tear line just below binding
  int tearY = y + bindingH + 1;
  int zigW = 20;
  for (int zx = x; zx < x + size - 1; zx += zigW) {
    epd_draw_line(zx, tearY, zx + zigW / 2, tearY + tearH, EPD_BLACK, fb);
    epd_draw_line(zx + zigW / 2, tearY + tearH, zx + zigW, tearY, EPD_BLACK, fb);
  }

  // Day name (extra top padding)
  int nameY = y + bindingH + tearH + 44;
  drawCenteredText(dayName(day.tm_wday), x + size / 2, nameY,
                   (GFXfont*)&Genty24, fb);

  // Large day number (extra space below day name)
  char numStr[8];
  snprintf(numStr, sizeof(numStr), "%d", day.tm_mday);
  int numberY = nameY + 118;
  drawCenteredText(numStr, x + size / 2, numberY, (GFXfont*)&Genty48, fb);
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------
static void getWindowRect(int& winX, int& winY, int& winW, int& winH) {
  winX = WIN_MARGIN;
  winY = WIN_MARGIN;
  winW = EPD_WIDTH  - WIN_MARGIN * 2;
  winH = EPD_HEIGHT - WIN_MARGIN * 2;
}

static void getContentRect(int& cx, int& cy, int& cw, int& ch) {
  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);
  cx = winX + WIN_MARGIN;
  cy = winY + WIN_MARGIN;
  cw = winW - WIN_MARGIN * 2;
  ch = winH - HEADER_H - WIN_MARGIN * 3;
}

static int getHeaderY() {
  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);
  return winY + winH - WIN_MARGIN - HEADER_H;
}

// ---------------------------------------------------------------------------
// Touch handling — single poll-driven state machine
// ---------------------------------------------------------------------------
// main.cpp calls updateTouch() every loop iteration. The state machine
// tracks IDLE → DOWN (finger on panel) → IDLE once the finger is lifted
// and the gesture is classified. This avoids the old touchDown/touchUp
// pair model which lost events when polls were throttled.
// ---------------------------------------------------------------------------

static bool isInCooldown() {
  return (long)(millis() - s_cooldownUntilMs) < 0;
}

static void triggerNav(int days) {
  s_baseDayOffset += days;
  s_pendingRender = true;
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
}

static void triggerScreenChange(Screen next, int dayOffset) {
  s_screen = next;
  s_baseDayOffset = dayOffset;
  s_pendingRender = true;
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
}

static void getHeaderNavRects(int& leftX, int& leftY, int& rightX, int& rightY,
                              int& arrowSize, int& headerY) {
  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);
  headerY = getHeaderY();
  arrowSize = 44;
  leftX  = winX + 20;
  rightX = winX + winW - 20 - arrowSize;
  leftY = rightY = headerY + (HEADER_H - arrowSize) / 2;
}

static bool hitHeaderArrow(int16_t x, int16_t y, bool left) {
  int leftX, leftY, rightX, rightY, arrowSize, headerY;
  getHeaderNavRects(leftX, leftY, rightX, rightY, arrowSize, headerY);
  int ax = left ? leftX : rightX;
  int ay = leftY;
  return (y >= ay && y <= ay + arrowSize &&
          x >= ax && x <= ax + arrowSize);
}

static bool inHeader(int16_t y) {
  int headerY = getHeaderY();
  return y >= headerY && y <= headerY + HEADER_H;
}

static bool hitBackButton(int16_t x, int16_t y) {
  if (s_screen != SCREEN_DAILY) return false;
  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);

  // Content starts below the status strip — must match renderDailyView()
  int contentTop = cy + DAILY_STATUS_H + 8;
  int pageX = cx + DAILY_PAGE_MARGIN;
  int bx = pageX;
  int by = contentTop + DAILY_PAGE_SIZE + DAILY_BACK_GAP;
  return x >= bx && x <= bx + DAILY_BACK_W && y >= by && y <= by + DAILY_BACK_H;
}

// Classify a completed touch gesture and fire the appropriate action.
static void classifyGesture() {
  unsigned long dur = millis() - s_startMs;
  int16_t dx = s_lastX - s_startX;
  int16_t dy = s_lastY - s_startY;
  int16_t absDx = abs(dx);
  int16_t absDy = abs(dy);
  bool isTap = absDx < SWIPE_MIN_PX && absDy < SWIPE_MIN_PX;

  // Swipe (horizontal, dominant in header area)
  if (inHeader(s_startY) && absDx > absDy && absDx >= SWIPE_MIN_PX &&
      dur < SWIPE_MAX_MS) {
    triggerNav(dx < 0 ? +1 : -1);
    return;
  }

  if (!isTap) return;
  if (dur > TAP_MAX_MS) return;

  // Back button on daily view
  if (s_screen == SCREEN_DAILY && hitBackButton(s_startX, s_startY)) {
    triggerScreenChange(SCREEN_WEEKLY, s_baseDayOffset);
    return;
  }

  // Header arrow taps
  if (inHeader(s_startY)) {
    if (hitHeaderArrow(s_startX, s_startY, true)) {
      triggerNav(-1);
      return;
    }
    if (hitHeaderArrow(s_startX, s_startY, false)) {
      triggerNav(+1);
      return;
    }
  }

  // Weekly column tap → open daily view
  if (s_screen == SCREEN_WEEKLY && !inHeader(s_startY)) {
    int cx, cy, cw, ch;
    getContentRect(cx, cy, cw, ch);
    if (s_startY >= cy && s_startY <= cy + ch &&
        s_startX >= cx && s_startX <= cx + cw) {
      constexpr int COL_GAP = 8;
      int colW = (cw - COL_GAP * (NUM_COLS - 1)) / NUM_COLS;
      int col = (s_startX - cx) / (colW + COL_GAP);
      if (col < 0) col = 0;
      if (col >= NUM_COLS) col = NUM_COLS - 1;
      int colLeft = cx + col * (colW + COL_GAP);
      if (s_startX <= colLeft + colW) {
        triggerScreenChange(SCREEN_DAILY, s_baseDayOffset + col);
        return;
      }
    }
  }
}

// Called every loop iteration by main.cpp.
// isTouched: true if finger is physically on the panel (INT pin low).
// x, y: valid only when isTouched is true.
void updateTouch(bool isTouched, int16_t x, int16_t y) {
  if (isInCooldown()) {
    if (!isTouched) s_phase = TOUCH_IDLE;
    return;
  }

  switch (s_phase) {
    case TOUCH_IDLE:
      if (isTouched) {
        s_phase = TOUCH_DOWN;
        s_startX = s_lastX = x;
        s_startY = s_lastY = y;
        s_startMs = millis();
      }
      break;

    case TOUCH_DOWN:
      if (isTouched) {
        s_lastX = x;
        s_lastY = y;
      } else {
        classifyGesture();
        s_phase = TOUCH_IDLE;
      }
      break;
  }
}

bool needsRender() {
  if (s_pendingRender) {
    s_pendingRender = false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Bottom header drawing
// ---------------------------------------------------------------------------
static void drawBottomHeader(const char* title, uint8_t* fb) {
  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);
  int headerY = getHeaderY();

  // Striped bar
  epd_fill_rect(winX + 1, headerY, winW - 2, HEADER_H, EPD_LTGRAY, fb);
  for (int i = 0; i < HEADER_H; i += 2) {
    epd_draw_hline(winX + 1, headerY + i, winW - 2, EPD_WHITE, fb);
  }
  epd_draw_rect(winX + 1, headerY, winW - 2, HEADER_H, EPD_BLACK, fb);

  // Arrow buttons
  int leftX, leftY, rightX, rightY, arrowSize, _;
  getHeaderNavRects(leftX, leftY, rightX, rightY, arrowSize, _);
  drawArrowButton(leftX,  leftY,  arrowSize, arrowSize, true,  fb);
  drawArrowButton(rightX, rightY, arrowSize, arrowSize, false, fb);

  // Battery (bigger: 48x24)
  int batW = 48, batH = 24;
  int batX = rightX - 20 - batW;
  int batY = headerY + (HEADER_H - batH) / 2;
  int batPercent = battery::lastPercent();
  if (batPercent < 0) batPercent = 0;
  drawBatteryIcon(batX, batY, batW, batH, batPercent, fb);

  // Title centered between left arrow and battery
  int titleCx = (leftX + arrowSize + batX) / 2;
  drawCenteredText(title, titleCx, headerY + 56, (GFXfont*)&Genty20, fb);
}

// ---------------------------------------------------------------------------
// Demo 1: rolling 4-day weekly view
// ---------------------------------------------------------------------------
static void renderWeeklyView() {
  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);

  epd_draw_rect(winX, winY, winW, winH, EPD_BLACK, fb);

  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);
  epd_draw_rect(cx, cy, cw, ch, EPD_BLACK, fb);

  constexpr int COL_GAP = 8;
  int colW = (cw - COL_GAP * (NUM_COLS - 1)) / NUM_COLS;

  constexpr int DAY_MINUTES = (DAY_END_HOUR - DAY_START_HOUR) * 60;

  struct tm base;
  getDemoBaseDay(base);
  base.tm_mday += s_baseDayOffset;
  mktime(&base);

  int headerH = 44;
  int timelineTop = cy + headerH;
  int timelineH = ch - headerH - 42;
  int allDayY = timelineTop + timelineH + 6;

  for (int i = 0; i < NUM_COLS; i++) {
    struct tm day = base;
    day.tm_mday += i;
    mktime(&day);
    int x = cx + i * (colW + COL_GAP);

    epd_draw_rect(x, cy, colW, ch, EPD_BLACK, fb);

    char headerStr[20];
    snprintf(headerStr, sizeof(headerStr), "%s %02d/%02d",
             dayNameShort(day.tm_wday), day.tm_mon + 1, day.tm_mday);
    drawCenteredText(headerStr, x + colW / 2, cy + 26, (GFXfont*)&Genty16, fb);

    epd_draw_hline(x + 6, timelineTop - 4, colW - 12, EPD_BLACK, fb);

    for (int h = DAY_START_HOUR; h <= DAY_END_HOUR; h += 3) {
      int minFromStart = (h - DAY_START_HOUR) * 60;
      int yy = timelineTop + (minFromStart * timelineH) / DAY_MINUTES;
      epd_draw_hline(x + 8, yy, colW - 16, EPD_LTGRAY, fb);
    }

    char dateStr[12];
    dateToString(day, dateStr, sizeof(dateStr));
    int colIndices[32];
    int colCount = eventsForDate(dateStr, colIndices, 32);

    for (int e = 0; e < colCount; e++) {
      const CalendarEvent& ev = s_events[colIndices[e]];

      if (ev.allDay) {
        epd_fill_rect(x + 4, allDayY, colW - 8, 30, EPD_BLACK, fb);
        epd_draw_rect(x + 4, allDayY, colW - 8, 30, EPD_BLACK, fb);
        // Truncate all-day title to fit the banner width
        char allDayTitle[48];
        int allDayMaxChars = (colW - 16) / 12;
        if (allDayMaxChars < 3) allDayMaxChars = 3;
        truncateWithEllipsis(ev.title, allDayTitle, sizeof(allDayTitle), allDayMaxChars);
        drawCenteredTextColored(allDayTitle, x + colW / 2, allDayY + 20,
                                (GFXfont*)&MeltSwashes14, fb, C_WHITE, C_BLACK);
        continue;
      }

      int startMin = ev.startHour * 60 + ev.startMin;
      int endMin   = startMin + ev.durationMin;
      int visStart = max(startMin, DAY_START_HOUR * 60);
      int visEnd   = min(endMin, DAY_END_HOUR * 60);
      int visDur   = visEnd - visStart;
      if (visDur <= 0) continue;

      int blockY = timelineTop + ((visStart - DAY_START_HOUR * 60) * timelineH) / DAY_MINUTES;
      int blockH = max(64, (visDur * timelineH) / DAY_MINUTES);
      if (blockY + blockH > allDayY - 4) blockH = allDayY - 4 - blockY;

      int blockX = x + 6;
      int blockW = colW - 12;

      epd_fill_rect(blockX, blockY, blockW, blockH, ev.shade << 4, fb);
      epd_draw_rect(blockX, blockY, blockW, blockH, EPD_BLACK, fb);

      char timeStr[16];
      formatTime(ev.startHour, ev.startMin, timeStr, sizeof(timeStr));
      FontProperties props;
      props.fg_color = (ev.shade <= C_MDGRAY) ? C_WHITE : C_BLACK;
      props.bg_color = ev.shade;
      props.flags = 0;
      props.fallback_glyph = 0;

      int32_t ttcx = blockX + 4, ttcy = blockY + 18;
      write_mode((GFXfont*)&MeltSwashes14, timeStr, &ttcx, &ttcy, fb, BLACK_ON_WHITE, &props);

      char titleBuf[48];
      int maxTitleChars = (blockW - 8) / 12;
      if (maxTitleChars < 4) maxTitleChars = 4;
      truncateWithEllipsis(ev.title, titleBuf, sizeof(titleBuf), maxTitleChars);
      drawTextColored((GFXfont*)&MeltSwashes14, titleBuf,
                      blockX + 4, blockY + 40, fb,
                      props.fg_color, props.bg_color);
    }
  }

  // Bottom header title: date range of the 4 days
  char titleBuf[48];
  struct tm firstDay = base;
  struct tm lastDay  = base;
  lastDay.tm_mday += NUM_COLS - 1;
  mktime(&lastDay);
  snprintf(titleBuf, sizeof(titleBuf), "%s %d – %s %d",
           monthName(firstDay.tm_mon), firstDay.tm_mday,
           monthName(lastDay.tm_mon),  lastDay.tm_mday);
  drawBottomHeader(titleBuf, fb);
}

// ---------------------------------------------------------------------------
// Demo 2: daily chronological list view
// ---------------------------------------------------------------------------
static int eventSortKey(const CalendarEvent& ev) {
  if (ev.allDay) return -1;
  return ev.startHour * 60 + ev.startMin;
}

static void renderDailyView() {
  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);

  epd_draw_rect(winX, winY, winW, winH, EPD_BLACK, fb);

  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);

  struct tm day;
  getDemoBaseDay(day);
  day.tm_mday += s_baseDayOffset;
  mktime(&day);

  // -- Collect and sort today's events -------------------------------
  char dateStr[12];
  dateToString(day, dateStr, sizeof(dateStr));
  int indices[32];
  int count = eventsForDate(dateStr, indices, 32);
  int timedCount = 0, allDayCount = 0;
  for (int e = 0; e < count; e++) {
    if (s_events[indices[e]].allDay) allDayCount++;
    else timedCount++;
  }
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - 1 - i; j++) {
      if (eventSortKey(s_events[indices[j]]) > eventSortKey(s_events[indices[j+1]])) {
        int tmp = indices[j];
        indices[j] = indices[j+1];
        indices[j+1] = tmp;
      }
    }
  }

  // -- Status strip: "Events: N | All-day: N" -----------------------
  int statusY = cy;
  epd_fill_rect(cx, statusY, cw, DAILY_STATUS_H, EPD_LTGRAY, fb);
  epd_draw_rect(cx, statusY, cw, DAILY_STATUS_H, EPD_BLACK, fb);
  char statusBuf[48];
  snprintf(statusBuf, sizeof(statusBuf), "Events: %d | All-day: %d",
           timedCount, allDayCount);
  {
    FontProperties sp;
    sp.fg_color = C_BLACK;
    sp.bg_color = C_LTGRAY;
    sp.flags = 0;
    sp.fallback_glyph = 0;
    int32_t sx = cx + 12, sy = statusY + 22;
    write_mode((GFXfont*)&MeltSwashes14, statusBuf, &sx, &sy, fb, BLACK_ON_WHITE, &sp);
  }

  // -- Content area starts below the status strip -------------------
  int contentTop = statusY + DAILY_STATUS_H + 8;

  // -- Tear-off calendar page (left column) -------------------------
  int pageX = cx + DAILY_PAGE_MARGIN;
  int pageY = contentTop;
  drawTearOffCalendar(pageX, pageY, DAILY_PAGE_SIZE, day, fb);

  // -- Back button below the calendar -------------------------------
  int backX = pageX;
  int backY = pageY + DAILY_PAGE_SIZE + DAILY_BACK_GAP;
  epd_fill_rect(backX, backY, DAILY_BACK_W, DAILY_BACK_H, EPD_WHITE, fb);
  epd_draw_rect(backX, backY, DAILY_BACK_W, DAILY_BACK_H, EPD_BLACK, fb);
  {
    int32_t btx = backX + 12, bty = backY + 32;
    writeln((GFXfont*)&MeltSwashes16, "< Back to Week", &btx, &bty, fb);
  }

  // -- Event list (right column) ------------------------------------
  int listX = pageX + DAILY_PAGE_SIZE + DAILY_LIST_GAP;
  int listY = contentTop;
  int listW = cx + cw - DAILY_PAGE_MARGIN - listX;
  int listH = ch - (contentTop - cy) - 8;
  int lineH = DAILY_ROW_H;

  int displayed = 0;
  int maxRows = listH / lineH;

  for (int i = 0; i < count; i++) {
    if (displayed >= maxRows) {
      int remaining = count - displayed;
      char moreBuf[32];
      snprintf(moreBuf, sizeof(moreBuf), "+%d more...", remaining);
      int32_t mx = listX + 10, my = listY + 20;
      writeln((GFXfont*)&MeltSwashes16, moreBuf, &mx, &my, fb);
      break;
    }

    const CalendarEvent& ev = s_events[indices[i]];
    uint8_t rowBg = (displayed % 2 == 0) ? C_LTGRAY : C_WHITE;

    if (displayed % 2 == 0) {
      epd_fill_rect(listX, listY, listW, lineH, EPD_LTGRAY, fb);
    }
    epd_draw_rect(listX, listY, listW, lineH, EPD_BLACK, fb);

    FontProperties rowProps;
    rowProps.fg_color = C_BLACK;
    rowProps.bg_color = rowBg;
    rowProps.flags = 0;
    rowProps.fallback_glyph = 0;

    if (ev.allDay) {
      // All-day: "ALL DAY" label on top, title below in Genty20
      drawTextColored((GFXfont*)&MeltSwashes14, "ALL DAY",
                      listX + 12, listY + 22, fb, C_BLACK, rowBg);

      char lineBuf[80];
      snprintf(lineBuf, sizeof(lineBuf), "%s | %s", ev.title, ev.location);
      char outBuf[64];
      truncateWithEllipsis(lineBuf, outBuf, sizeof(outBuf), (listW - 24) / 12);
      int32_t titleX = listX + 12, titleY = listY + 52;
      write_mode((GFXfont*)&Genty20, outBuf, &titleX, &titleY, fb, BLACK_ON_WHITE, &rowProps);

      char descBuf[64];
      truncateWithEllipsis(ev.description, descBuf, sizeof(descBuf), (listW - 24) / 12);
      drawTextColored((GFXfont*)&MeltSwashes14, descBuf,
                      listX + 12, listY + 80, fb, C_BLACK, rowBg);
    } else {
      // Timed: time range on top in MeltSwashes16, title below in Genty20
      int endMin = ev.startHour * 60 + ev.startMin + ev.durationMin;
      int eh = endMin / 60;
      int emin = endMin % 60;
      char startStr[16], endStr[16];
      formatTime(ev.startHour, ev.startMin, startStr, sizeof(startStr));
      formatTime(eh, emin, endStr, sizeof(endStr));
      char timeStr[40];
      snprintf(timeStr, sizeof(timeStr), "%s - %s", startStr, endStr);

      drawTextColored((GFXfont*)&MeltSwashes16, timeStr,
                      listX + 12, listY + 24, fb, C_BLACK, rowBg);

      char lineBuf[80];
      snprintf(lineBuf, sizeof(lineBuf), "%s | %s", ev.title, ev.location);
      char outBuf[64];
      truncateWithEllipsis(lineBuf, outBuf, sizeof(outBuf), (listW - 24) / 12);
      int32_t titleX = listX + 12, titleY = listY + 54;
      write_mode((GFXfont*)&Genty20, outBuf, &titleX, &titleY, fb, BLACK_ON_WHITE, &rowProps);

      char descBuf[64];
      truncateWithEllipsis(ev.description, descBuf, sizeof(descBuf), (listW - 24) / 12);
      drawTextColored((GFXfont*)&MeltSwashes14, descBuf,
                      listX + 12, listY + 82, fb, C_BLACK, rowBg);
    }

    listY += lineH;
    displayed++;
  }

  // -- Bottom header title: "Today" / "Tomorrow" / "Friday (+2D)" ---
  char titleBuf[40];
  if (s_baseDayOffset == 0) {
    snprintf(titleBuf, sizeof(titleBuf), "Today");
  } else if (s_baseDayOffset == 1) {
    snprintf(titleBuf, sizeof(titleBuf), "Tomorrow");
  } else {
    snprintf(titleBuf, sizeof(titleBuf), "%s (+%dD)",
             dayName(day.tm_wday), s_baseDayOffset);
  }
  drawBottomHeader(titleBuf, fb);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void render() {
  if (s_events == nullptr || s_eventCount == 0) {
    uint8_t* fb = display_mgr::framebuffer();
    memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
    const char* msg = "Waiting for data...";
    int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
    int32_t tcx = EPD_WIDTH / 2, tcy = EPD_HEIGHT / 2;
    get_text_bounds((GFXfont*)&Genty24, msg, &tcx, &tcy, &tx1, &ty1, &tw, &th, NULL);
    tcx = EPD_WIDTH / 2 - tw / 2;
    tcy = EPD_HEIGHT / 2;
    writeln((GFXfont*)&Genty24, msg, &tcx, &tcy, fb);
    return;
  }

  switch (s_screen) {
    case SCREEN_WEEKLY: renderWeeklyView(); break;
    case SCREEN_DAILY:  renderDailyView();  break;
    default:          renderWeeklyView(); break;
  }
}

void setEvents(const CalendarEvent* events, int count) {
  s_events = events;
  s_eventCount = count;
  s_pendingRender = true;
}

void next() {
  s_screen = (Screen)((s_screen + 1) % SCREEN_COUNT);
}

} // namespace ui
