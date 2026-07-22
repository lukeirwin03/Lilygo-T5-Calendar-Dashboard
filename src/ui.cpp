#include "ui.h"
#include "dashboards/calendar_dashboard.h"
#include "display_manager.h"
#include "battery.h"
#include "epd_driver.h"
#include "fonts/Genty20pt7b.h"
#include "fonts/Genty24pt7b.h"
#include "fonts/Genty32pt7b.h"
#include "fonts/Genty48pt7b.h"
#include "fonts/MeltSwashes14pt7b.h"
#include "fonts/MeltSwashes16pt7b.h"
#include "fonts/MeltSwashes18pt7b.h"
#include "fonts/MeltSwashes20pt7b.h"
#ifndef ENV_DEMO
#include "ui_settings.h"
#include "settings.h"
#endif
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
static constexpr uint8_t EPD_MDGRAY = C_MDGRAY << 4;
static constexpr uint8_t EPD_LTGRAY = C_LTGRAY << 4;
static constexpr uint8_t EPD_WHITE  = C_WHITE  << 4;

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr int WIN_MARGIN     = 12;
static constexpr int HEADER_H       = 90;
static constexpr int DAY_START_HOUR = 7;
static constexpr int DAY_END_HOUR   = 22;

// Phase 10 focus+context layout: 3 columns, focus is 2× the width of context.
// Computed: total = 2*context + focus + 2*gap = 4*context + 2*gap
// context_w = (total - 2*gap) / 4
static constexpr int FOCUS_COL_RATIO = 2;  // focus is N× the context width

static constexpr int EVENT_GAP = 4;  // px of white space between adjacent event blocks

static constexpr int MIN_BLOCK_HEIGHT = 76;  // px; tuned for MeltSwashes18 time + MeltSwashes16 title

static constexpr int SHORT_EVENT_THRESHOLD = 60;   // min; events <= this use 1-line format
static constexpr int SHORT_EVENT_MIN_H     = 28;   // px; 1-line block min height
static constexpr int SHORT_EVENT_MAX_H     = 50;   // px; 1-line block max height (at 60 min)
static constexpr int LONG_EVENT_CAP_MIN    = 180;  // min; events this long or longer cap here

// Daily view layout — shared between renderDailyView() and hitBackButton()
// to prevent coordinate mismatches.
static constexpr int DAILY_PAGE_SIZE    = 280;  // tear-off calendar square
static constexpr int DAILY_PAGE_MARGIN  = 16;   // offset from content rect
static constexpr int DAILY_BACK_W       = 280;  // back button width (matches DAILY_PAGE_SIZE)
static constexpr int DAILY_BACK_H       = 48;   // back button height
static constexpr int DAILY_BACK_GAP     = 12;   // gap between calendar and back button
static constexpr int DAILY_LIST_GAP     = 24;   // gap between calendar column and list
static constexpr int DAILY_ROW_H        = 100;  // event list row height (per ui.txt)
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
static Screen s_prevScreen = SCREEN_WEEKLY;  // restored on settings close
static int        s_baseDayOffset = 0;

static bool       s_pendingRender = false;

// Refresh mode: full or partial (settings row update).
enum RefreshMode { REFRESH_FULL, REFRESH_PARTIAL_SETTINGS };
static RefreshMode s_refreshMode = REFRESH_FULL;

// Last action description for diagnostics.
static char s_lastAction[128] = "";

// Settings screen tracks the previous touch state so a single tap is handled
// only on the rising edge of the touch signal.
static bool s_wasSettingsTouched = false;

// Touch state machine
enum TouchPhase { TOUCH_IDLE, TOUCH_DOWN };
static TouchPhase s_phase = TOUCH_IDLE;
static int16_t    s_startX = 0;
static int16_t    s_startY = 0;
static int16_t    s_lastX  = 0;
static int16_t    s_lastY  = 0;
static unsigned long s_startMs = 0;
static unsigned long s_cooldownUntilMs = 0;

// Debug mode unlocks the Settings screen. Persisted across deep sleep so the
// user only has to enable it once. Toggled by long-pressing the battery icon.
#ifndef ENV_DEMO
RTC_DATA_ATTR static bool s_debugMode = false;
#endif

static constexpr unsigned long TAP_MAX_MS         = 500;
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

// Returns true if column `colIdx` in the current view represents today.
// In the focus+context layout, colIdx 0 = focus-1, colIdx 1 = focus, colIdx 2 = focus+1.
// Today is at offset 0, so colIdx is today when s_baseDayOffset + (colIdx - 1) == 0.
static bool isTodayColumn(int colIdx) {
  return (s_baseDayOffset + (colIdx - 1)) == 0;
}

// Resolve display settings from the runtime settings struct when available,
// otherwise fall back to compile-time defaults. The demo env excludes
// settings.cpp so we use the constexpr fallbacks there.
static int resolvedDayStartHour() {
#ifndef ENV_DEMO
  return settings::get().day_start_hour;
#else
  return DAY_START_HOUR;
#endif
}
static int resolvedDayEndHour() {
#ifndef ENV_DEMO
  return settings::get().day_end_hour;
#else
  return DAY_END_HOUR;
#endif
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

// Returns true if any other event on the same day as `ev` starts strictly
// after `ev` starts but before `ev` ends. Used to suppress the end-time
// display when a following event's block would visually collide with the
// area where the end time would be drawn.
static bool hasOverlappingFollower(const CalendarEvent& ev) {
  int evStart = ev.startHour * 60 + ev.startMin;
  int evEnd = evStart + ev.durationMin;
  for (int i = 0; i < s_eventCount; i++) {
    if (i == (&ev - s_events)) continue;  // skip self (pointer arithmetic)
    if (strcmp(s_events[i].date, ev.date) != 0) continue;
    if (s_events[i].allDay) continue;
    int otherStart = s_events[i].startHour * 60 + s_events[i].startMin;
    if (otherStart > evStart && otherStart < evEnd) {
      return true;
    }
  }
  return false;
}

// Returns true if there's an all-day event with the given title on the given
// date (YYYY-MM-DD). Used to detect multi-day event continuation for arrows.
static bool hasAllDayEventOnDate(const char* dateStr, const char* title) {
  if (!dateStr || !title) return false;
  for (int i = 0; i < s_eventCount; i++) {
    if (s_events[i].allDay
        && strcmp(s_events[i].date, dateStr) == 0
        && strcmp(s_events[i].title, title) == 0) {
      return true;
    }
  }
  return false;
}

static void formatTime(int hour, int min, char* buf, size_t len) {
  int h = hour % 12;
  if (h == 0) h = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  snprintf(buf, len, "%d:%02d %s", h, min, ampm);
}

// Compact time format: "3:30PM" (no space before AM/PM). Used in the
// 1-line short-event format where horizontal space is at a premium.
static void formatTimeCompact(int hour, int min, char* buf, size_t len) {
  int h = hour % 12;
  if (h == 0) h = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  snprintf(buf, len, "%d:%02d%s", h, min, ampm);
}

// Format a time range like "3:30 - 4:00 PM" (compact, same AM/PM) or
// "11:30 AM - 1:00 PM" (verbose, different AM/PM). Handles midnight wrap
// via modulo 24. Output buffer must be at least 24 bytes.
static void formatTimeRange(int startHour, int startMin, int durationMin,
                            char* buf, size_t len) {
  int startTotal = startHour * 60 + startMin;
  int endTotal = startTotal + durationMin;
  int eh = (endTotal / 60) % 24;
  int em = endTotal % 60;

  // 12-hour conversions
  int sh12 = startHour % 12;  if (sh12 == 0) sh12 = 12;
  int eh12 = eh % 12;          if (eh12 == 0) eh12 = 12;
  const char* sap = (startHour < 12) ? "AM" : "PM";
  const char* eap = (eh < 12) ? "AM" : "PM";

  if (sap == eap) {
    // Same half of day — show AM/PM only once at the end
    snprintf(buf, len, "%d:%02d - %d:%02d %s", sh12, startMin, eh12, em, eap);
  } else {
    // Different halves — show each with its own AM/PM
    snprintf(buf, len, "%d:%02d %s - %d:%02d %s",
             sh12, startMin, sap, eh12, em, eap);
  }
}

// Compute the block height for a timed event given its duration.
// - Short events (<= SHORT_EVENT_THRESHOLD): 1-line format, proportional
//   in the SHORT_EVENT_MIN_H..SHORT_EVENT_MAX_H range (clamped to 30 min floor).
// - Longer events: 2-line format, larger of MIN_BLOCK_HEIGHT and proportional, with
//   the proportional value capped at LONG_EVENT_CAP_MIN so events longer than
//   3 hr render at the same height (the user's "consistent size above 3 hr" rule).
static int computeBlockHeight(int durationMin, int timelineH, int dayMinutes) {
  if (durationMin <= SHORT_EVENT_THRESHOLD) {
    int clamped = max(30, durationMin);
    return SHORT_EVENT_MIN_H
           + (clamped - 30) * (SHORT_EVENT_MAX_H - SHORT_EVENT_MIN_H) / 30;
  }
  int cappedDur = min(durationMin, LONG_EVENT_CAP_MIN);
  int proportional = (cappedDur * timelineH) / dayMinutes;
  return max(MIN_BLOCK_HEIGHT, proportional);
}

// Collect event indices for a date, logging when the per-day cap is hit.
static int eventsForDateLogged(const char* dateStr, int* outIndices, int maxIndices) {
  int total = 0;
  for (int i = 0; i < s_eventCount; i++) {
    if (strcmp(s_events[i].date, dateStr) == 0) total++;
  }
  int n = eventsForDate(dateStr, outIndices, maxIndices);
  if (total > n) {
    Serial.printf("[ui] WARN: %d events on %s but only rendering %d (cap)\n",
                  total, dateStr, n);
  }
  return n;
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

// Measure the rendered width of `str` in `font` using get_text_bounds.
// Returns the width in pixels.
static int measureTextWidth(GFXfont* font, const char* str) {
  int32_t x = 0, y = 0, x1 = 0, y1 = 0, w = 0, h = 0;
  get_text_bounds(font, str, &x, &y, &x1, &y1, &w, &h, NULL);
  return w;
}

// Copy `str` into `out` (capacity outLen), truncating with "..." if the
// rendered width in `font` exceeds maxWidth. Walks backward one char at a
// time from the end until the result (including "...") fits. Always
// null-terminates. If `str` already fits, copies it whole.
//
// Replaces the old chars * 12px estimate which was wrong at larger sizes.
static void truncateToFitWidth(GFXfont* font, const char* str, int maxWidth,
                               char* out, size_t outLen) {
  if (!str || !out || outLen < 4) {
    if (out && outLen > 0) out[0] = '\0';
    return;
  }

  int fullW = measureTextWidth(font, str);
  if (fullW <= maxWidth) {
    strlcpy(out, str, outLen);
    return;
  }

  // Walk backward from the end of str until "prefix..." fits.
  size_t len = strlen(str);
  size_t copyLen = len;
  while (copyLen > 1) {
    copyLen--;
    if (copyLen + 4 > outLen) continue;  // need room for copyLen + "..." + '\0'
    strlcpy(out, str, copyLen + 1);      // copy first copyLen chars + '\0'
    strlcat(out, "...", outLen);
    if (measureTextWidth(font, out) <= maxWidth) {
      // Prefer cutting at a word boundary instead of mid-word. Strip trailing
      // spaces, then if the original string continues with a non-space char
      // (meaning the prefix broke a word), walk back to the previous space.
      // Only keep the word-boundary version if it retains at least 80% of the
      // character-by-character length; otherwise the truncation is too aggressive
      // and we keep the longer mid-word cut.
      size_t charLen = strlen(out) - 3;
      while (charLen > 0 && out[charLen - 1] == ' ') {
        charLen--;
      }
      out[charLen] = '\0';

      if (str[copyLen] != ' ' && str[copyLen] != '\0' && charLen >= 4) {
        size_t boundary = charLen;
        while (boundary > 3 && out[boundary - 1] != ' ') {
          boundary--;
        }
        if (boundary > 3) {
          size_t wordLen = boundary - 1;
          if (wordLen >= charLen * 4 / 5) {
            out[wordLen] = '\0';
          }
        }
      }
      strlcat(out, "...", outLen);
      return;
    }
  }

  // Nothing fit — just emit "..." (or as much as fits).
  strlcpy(out, "...", outLen);
}

// Word-wrapped text result. Up to 3 lines, each up to 64 chars.
struct WrappedText {
  char lines[3][64];
  int count;  // actual number of lines produced (1..3); 0 if input was empty
};

// Word-wrap `str` to fit `maxWidth` pixels in `font`, producing up to `maxLines`
// lines. Greedy fill: walk word by word, flush a line when adding the next
// word would exceed maxWidth. When the text doesn't fully fit in maxLines,
// the last line is truncated at a word boundary with "...". Each line in the
// result is null-terminated.
//
// Edge cases handled:
//   - Empty or null input -> returns count=0
//   - Single word longer than maxWidth -> that word is char-truncated via
//     truncateToFitWidth on its own line
//   - Text fully fits in fewer than maxLines -> returns the actual count
static WrappedText wrapText(GFXfont* font, const char* str, int maxWidth, int maxLines) {
  WrappedText result = {};
  if (!str || !str[0] || maxLines <= 0 || maxLines > 3) return result;

  size_t len = strlen(str);
  size_t i = 0;
  while (i < len && str[i] == ' ') i++;  // skip leading spaces

  int lineIdx = 0;
  size_t lineLen = 0;

  while (i < len && lineIdx < maxLines) {
    size_t wordStart = i;
    while (i < len && str[i] != ' ') i++;
    size_t wordEnd = i;
    size_t wordLen = wordEnd - wordStart;
    while (i < len && str[i] == ' ') i++;  // skip spaces after word

    char candidate[64];

    if (lineLen == 0) {
      if (wordLen >= sizeof(candidate)) {
        // Word alone exceeds the line buffer — char-truncate it directly.
        char wordBuf[128];
        size_t copyLen = wordLen < sizeof(wordBuf) ? wordLen : sizeof(wordBuf) - 1;
        memcpy(wordBuf, str + wordStart, copyLen);
        wordBuf[copyLen] = '\0';
        char truncated[64];
        truncateToFitWidth(font, wordBuf, maxWidth, truncated, sizeof(truncated));
        strlcpy(result.lines[lineIdx], truncated, sizeof(result.lines[lineIdx]));
        lineIdx++;
        lineLen = 0;
        continue;
      }
      memcpy(candidate, str + wordStart, wordLen);
      candidate[wordLen] = '\0';
    } else {
      if (lineLen + 1 + wordLen >= sizeof(candidate)) {
        // Word would overflow the line buffer — flush and retry on a new line.
        lineIdx++;
        if (lineIdx >= maxLines) {
          lineIdx = maxLines - 1;
          // Last line: maximize what fits using the full remaining text.
          char remaining[256];
          snprintf(remaining, sizeof(remaining), "%s %s",
                   result.lines[lineIdx], str + wordStart);
          truncateToFitWidth(font, remaining, maxWidth,
                             result.lines[lineIdx],
                             sizeof(result.lines[lineIdx]));
          result.count = maxLines;
          return result;
        }
        lineLen = 0;
        i = wordStart;  // re-process this word on the fresh line
        continue;
      }
      memcpy(candidate, result.lines[lineIdx], lineLen);
      candidate[lineLen] = ' ';
      memcpy(candidate + lineLen + 1, str + wordStart, wordLen);
      candidate[lineLen + 1 + wordLen] = '\0';
    }

    if (measureTextWidth(font, candidate) <= maxWidth) {
      strlcpy(result.lines[lineIdx], candidate, sizeof(result.lines[lineIdx]));
      lineLen = strlen(result.lines[lineIdx]);
      continue;
    }

    // Word doesn't fit on the current line.
    if (lineLen > 0) {
      // Flush current line and retry this word on a fresh line.
      lineIdx++;
      if (lineIdx >= maxLines) {
        lineIdx = maxLines - 1;
        // Last line: maximize what fits using the full remaining text.
        char remaining[256];
        snprintf(remaining, sizeof(remaining), "%s %s",
                 result.lines[lineIdx], str + wordStart);
        truncateToFitWidth(font, remaining, maxWidth,
                           result.lines[lineIdx],
                           sizeof(result.lines[lineIdx]));
        result.count = maxLines;
        return result;
      }
      lineLen = 0;
      i = wordStart;  // re-process this word on the fresh line
      continue;
    }

    // Empty line: single word doesn't fit — char-truncate it.
    char wordBuf[128];
    size_t copyLen = wordLen < sizeof(wordBuf) ? wordLen : sizeof(wordBuf) - 1;
    memcpy(wordBuf, str + wordStart, copyLen);
    wordBuf[copyLen] = '\0';
    char truncated[64];
    truncateToFitWidth(font, wordBuf, maxWidth, truncated, sizeof(truncated));
    strlcpy(result.lines[lineIdx], truncated, sizeof(result.lines[lineIdx]));

    lineIdx++;
    lineLen = 0;
  }

  if (lineLen > 0 && lineIdx < maxLines) {
    lineIdx++;
  }

  result.count = lineIdx;
  return result;
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

// Draw a horizontal dotted line at (y) across the column width, centered.
// Used as a "nothing happening" indicator in gaps between events.
// `dotColor` is the EPD-shifted shade to draw dots in (typically LTGRAY).
static void drawGapIndicator(int x, int y, int w, uint8_t dotColor, uint8_t* fb) {
  // Dotted pattern: 3px dot, 6px gap, repeating
  int dotW = 3;
  int gap = 6;
  int pattern = dotW + gap;
  int startX = x + (w % pattern) / 2;  // center the pattern
  for (int dx = 0; dx + dotW <= w; dx += pattern) {
    epd_fill_rect(startX + dx, y, dotW, 1, dotColor, fb);
  }
}

// Render text with a 1 px black outline around the fg_color fill. Used for
// white text on dark fills where bare white lacks edge definition. The
// patched font renderer skips transparent pixels, so the 8 outline passes
// only paint where glyph pixels exist at each offset, leaving the original
// background untouched elsewhere.
static void drawTextWithOutline(GFXfont* font, const char* str, int x, int y,
                                uint8_t* fb, uint8_t fg_color, uint8_t bg_color) {
  FontProperties outlineProps;
  outlineProps.fg_color = C_BLACK;
  outlineProps.bg_color = bg_color;
  outlineProps.flags = 0;
  outlineProps.fallback_glyph = 0;

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;
      int32_t ox = x + dx, oy = y + dy;
      write_mode(font, str, &ox, &oy, fb, BLACK_ON_WHITE, &outlineProps);
    }
  }

  FontProperties mainProps;
  mainProps.fg_color = fg_color;
  mainProps.bg_color = bg_color;
  mainProps.flags = 0;
  mainProps.fallback_glyph = 0;
  int32_t mx = x, my = y;
  write_mode(font, str, &mx, &my, fb, BLACK_ON_WHITE, &mainProps);
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
  cy = winY + 4;                                // was winY + WIN_MARGIN — content starts 8 px higher
  cw = winW - WIN_MARGIN * 2;
  ch = winH - HEADER_H - WIN_MARGIN * 2 + 24;   // reclaim the 8 px from top + 4 px from the now-unnecessary top inset
}

static int getHeaderY() {
  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);
  return winY + winH - WIN_MARGIN / 2 - HEADER_H + 12;
}

// Focus column extends from cy to near the bottom of the screen.
// The footer (split into left/right sections) only spans the context columns.
static void getFocusColumnRect(int& fx, int& fy, int& fw, int& fh) {
  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);

  constexpr int COL_GAP = 8;
  int contextW = (cw - COL_GAP * 2) / 4;
  int focusW = contextW * 2;

  fx = cx + contextW + COL_GAP;
  fy = cy;
  fw = focusW;
  fh = (EPD_HEIGHT - 4) - fy;   // 4 px bottom margin
}

// Left footer section: under the left context column.
static void getLeftFooterRect(int& lx, int& ly, int& lw, int& lh) {
  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);
  constexpr int COL_GAP = 8;
  int contextW = (cw - COL_GAP * 2) / 4;

  lx = cx;
  ly = getHeaderY();
  lw = contextW;
  lh = HEADER_H;
}

// Right footer section: under the right context column.
static void getRightFooterRect(int& rx, int& ry, int& rw, int& rh) {
  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);
  constexpr int COL_GAP = 8;
  int contextW = (cw - COL_GAP * 2) / 4;
  int focusW = contextW * 2;

  rx = cx + contextW + COL_GAP + focusW + COL_GAP;
  ry = getHeaderY();
  rw = contextW;
  rh = HEADER_H;
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

static void enterSettings() {
  s_prevScreen = s_screen;
  s_screen = SCREEN_SETTINGS;
  s_pendingRender = true;
  s_refreshMode = REFRESH_FULL;
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
  snprintf(s_lastAction, sizeof(s_lastAction), "screen->settings");
}

static void triggerScreenChange(Screen next, int dayOffset) {
  s_screen = next;
  s_baseDayOffset = dayOffset;
  s_pendingRender = true;
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
}

// Hit test for an arrow in the split footer.
// `left` = true for back arrow (in left footer), false for forward arrow (in right footer).
static bool hitFooterArrow(int16_t x, int16_t y, bool left) {
  int arrowSize = 60;
  if (left) {
    int lx, ly, lw, lh;
    getLeftFooterRect(lx, ly, lw, lh);
    int arrowX = lx + 12;
    int arrowY = ly + (lh - arrowSize) / 2;
    return (x >= arrowX && x <= arrowX + arrowSize &&
            y >= arrowY && y <= arrowY + arrowSize);
  } else {
    int rx, ry, rw, rh;
    getRightFooterRect(rx, ry, rw, rh);
    int arrowX = rx + rw - 12 - arrowSize;
    int arrowY = ry + (rh - arrowSize) / 2;
    return (x >= arrowX && x <= arrowX + arrowSize &&
            y >= arrowY && y <= arrowY + arrowSize);
  }
}

// Hit test for the "Today" button in the left footer.
static bool hitTodayButton(int16_t x, int16_t y) {
  int lx, ly, lw, lh;
  getLeftFooterRect(lx, ly, lw, lh);

  // Compute the same geometry as drawLeftFooter()
  const char* todayLabel = "Today";
  int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
  int32_t tmcx = 0, tmcy = 0;
  get_text_bounds((GFXfont*)&MeltSwashes18, todayLabel, &tmcx, &tmcy, &tx1, &ty1, &tw, &th, NULL);
  int btnX = lx + lw - 12 - tw - 16;   // 12 px right padding, 16 px inner padding
  int btnY = ly + (lh - 32) / 2;       // 32 px tall button, vertically centered
  int btnW = tw + 32;
  int btnH = 32;
  return (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH);
}

// Returns true if (x, y) is inside EITHER footer section (left or right).
static bool inHeader(int16_t x, int16_t y) {
  int lx, ly, lw, lh;
  getLeftFooterRect(lx, ly, lw, lh);
  if (x >= lx && x <= lx + lw && y >= ly && y <= ly + lh) return true;
  int rx, ry, rw, rh;
  getRightFooterRect(rx, ry, rw, rh);
  return (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh);
}

// Battery icon rect, matching drawRightFooter(). Used for both the existing
// tap-target and the long-press-to-enable-debug gesture.
static bool hitBatteryIcon(int16_t x, int16_t y) {
  int rx, ry, rw, rh;
  getRightFooterRect(rx, ry, rw, rh);
  int batW = 48, batH = 24;
  int batX = rx + 12;
  int batY = ry + (rh - batH) / 2;
  return (x >= batX && x <= batX + batW && y >= batY && y <= batY + batH);
}

// Gear icon rect, shared between drawRightFooter() and the gear hit-test
// in classifyGesture(). Single source of truth so the tap target always
// matches the drawn icon.
static void getGearRect(int& gearX, int& gearY) {
  int rx, ry, rw, rh;
  getRightFooterRect(rx, ry, rw, rh);
  int batW = 48;
  int batX = rx + 12;
  gearX = batX + batW + 20;
  gearY = ry + (rh - 16) / 2;
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

  Serial.printf("[ui] CLASSIFY start=(%d,%d) last=(%d,%d) dx=%d dy=%d dur=%lums isTap=%d\n",
                s_startX, s_startY, s_lastX, s_lastY, dx, dy, dur, isTap);

  if (!isTap) {
    snprintf(s_lastAction, sizeof(s_lastAction), "ignored: moved (%d,%d) in %lums",
             dx, dy, dur);
    return;
  }

  // Hidden gesture: long-press the battery icon to toggle debug mode.
  // Debug mode reveals the gear icon / Settings screen.
#ifndef ENV_DEMO
  if (dur >= 2000 && hitBatteryIcon(s_startX, s_startY)) {
    s_debugMode = !s_debugMode;
    s_pendingRender = true;
    s_refreshMode = REFRESH_FULL;
    s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
    snprintf(s_lastAction, sizeof(s_lastAction),
             "debug mode %s", s_debugMode ? "ENABLED" : "disabled");
    return;
  }
#endif

  if (dur > TAP_MAX_MS) {
    snprintf(s_lastAction, sizeof(s_lastAction), "ignored: long press %lums at (%d,%d)",
             dur, s_startX, s_startY);
    return;
  }

  // Back button on daily view
  if (s_screen == SCREEN_DAILY && hitBackButton(s_startX, s_startY)) {
    triggerScreenChange(SCREEN_WEEKLY, s_baseDayOffset);
    return;
  }

  // When debug mode is on, a normal tap on the battery icon opens Settings.
#ifndef ENV_DEMO
  if (s_debugMode && hitBatteryIcon(s_startX, s_startY)) {
    enterSettings();
    return;
  }
#endif

  // Footer arrow taps and Today button
  if (inHeader(s_startX, s_startY)) {
    if (hitFooterArrow(s_startX, s_startY, true)) {
      triggerNav(-1);
      return;
    }
    if (hitFooterArrow(s_startX, s_startY, false)) {
      triggerNav(+1);
      return;
    }
    // Today button (left footer) — reset focus to today.
    if (hitTodayButton(s_startX, s_startY)) {
      int oldOffset = s_baseDayOffset;
      s_baseDayOffset = 0;
      s_pendingRender = true;
      s_refreshMode = REFRESH_FULL;
      s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
      snprintf(s_lastAction, sizeof(s_lastAction),
               "today button (was offset %d)", oldOffset);
      return;
    }
    // Gear icon (right footer, between battery and arrow)
    int gearX, gearY;
    getGearRect(gearX, gearY);
    // Hit target is the 16x16 drawn icon plus 2px padding on each side
    // for a forgiving tap zone.
    if (s_startX >= gearX - 2 && s_startX <= gearX + 18 &&
        s_startY >= gearY - 2 && s_startY <= gearY + 18) {
#ifndef ENV_DEMO
      if (s_debugMode) {
        enterSettings();
        return;
      }
#endif
    }

    // Tapped in footer but not on a button
    snprintf(s_lastAction, sizeof(s_lastAction), "missed: footer tap at (%d,%d)",
             s_startX, s_startY);
    return;
  }

  // Weekly column tap → open daily view
  if (s_screen == SCREEN_WEEKLY && !inHeader(s_startX, s_startY)) {
    int cx, cy, cw, ch;
    getContentRect(cx, cy, cw, ch);
    if (s_startY >= cy && s_startY <= cy + ch &&
        s_startX >= cx && s_startX <= cx + cw) {
      // Phase 10: focus+context layout has variable column widths.
      // Reconstruct the column index from x position.
      constexpr int COL_GAP = 8;
      int contextW = (cw - COL_GAP * 2) / 4;
      int focusW = contextW * 2;

      int col;
      if (s_startX < cx + contextW) {
        col = 0;  // left context
      } else if (s_startX < cx + contextW + COL_GAP + focusW) {
        col = 1;  // focus
      } else {
        col = 2;  // right context
      }

      if (col == 0) {
        // Left context column = back arrow (focus moves to yesterday's neighbor)
        triggerNav(-1);
        snprintf(s_lastAction + strlen(s_lastAction), sizeof(s_lastAction) - strlen(s_lastAction),
                 " [left context -> focus back]");
        return;
      } else if (col == 2) {
        // Right context column = forward arrow
        triggerNav(+1);
        snprintf(s_lastAction + strlen(s_lastAction), sizeof(s_lastAction) - strlen(s_lastAction),
                 " [right context -> focus fwd]");
        return;
      } else {
        // col == 1, focus column = open daily view for the focus day
        triggerScreenChange(SCREEN_DAILY, s_baseDayOffset);
        snprintf(s_lastAction + strlen(s_lastAction), sizeof(s_lastAction) - strlen(s_lastAction),
                 " [focus -> daily]");
        return;
      }
    }
  }
}

// Called every loop iteration by main.cpp.
// isTouched: true if finger is physically on the panel (INT pin low).
// x, y: valid only when isTouched is true.
void updateTouch(bool isTouched, int16_t x, int16_t y) {
  // Settings screen handles taps directly, not via the gesture state machine
#ifndef ENV_DEMO
  if (s_screen == SCREEN_SETTINGS) {
    if (isTouched && !s_wasSettingsTouched) {
      ui_settings::TapResult result = ui_settings::handleTap(x, y);
      if (result == ui_settings::TAP_CLOSE) {
        s_screen = s_prevScreen;
        s_pendingRender = true;
        s_refreshMode = REFRESH_FULL;
        s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
      } else if (result == ui_settings::TAP_FULL) {
        s_pendingRender = true;
        s_refreshMode = REFRESH_FULL;
      } else if (result == ui_settings::TAP_PARTIAL) {
        s_pendingRender = true;
        s_refreshMode = REFRESH_PARTIAL_SETTINGS;
      }
    }
    s_wasSettingsTouched = isTouched;
    return;
  }
#endif

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

int refreshMode() {
  return (int)s_refreshMode;
}

void getSettingsDirtyRect(int& x, int& y, int& w, int& h) {
#ifndef ENV_DEMO
  ui_settings::getDirtyRect(x, y, w, h);
#endif
}

// ---------------------------------------------------------------------------
// Split footer drawing
// ---------------------------------------------------------------------------
static void drawLeftFooter(uint8_t* fb) {
  int lx, ly, lw, lh;
  getLeftFooterRect(lx, ly, lw, lh);

  // Striped background (matches the old footer style for visual continuity).
  epd_fill_rect(lx + 1, ly, lw - 2, lh, EPD_LTGRAY, fb);
  for (int i = 0; i < lh; i += 2) {
    epd_draw_hline(lx + 1, ly + i, lw - 2, EPD_WHITE, fb);
  }
  epd_draw_rect(lx + 1, ly, lw - 2, lh, EPD_BLACK, fb);

  // Back arrow (left side of the left footer)
  int arrowSize = 60;
  int arrowX = lx + 12;
  int arrowY = ly + (lh - arrowSize) / 2;
  drawArrowButton(arrowX, arrowY, arrowSize, arrowSize, true, fb);

  // "Today" button (right side of the left footer) — small text label
  const char* todayLabel = "Today";
  int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
  int32_t tmcx = 0, tmcy = 0;
  get_text_bounds((GFXfont*)&MeltSwashes18, todayLabel, &tmcx, &tmcy, &tx1, &ty1, &tw, &th, NULL);
  int btnX = lx + lw - 12 - tw - 16;   // 12 px right padding, 16 px inner padding
  int btnY = ly + (lh - 32) / 2;       // 32 px tall button, vertically centered
  int btnW = tw + 32;
  int btnH = 32;
  epd_fill_rect(btnX, btnY, btnW, btnH, EPD_DKGRAY, fb);
  epd_draw_rect(btnX, btnY, btnW, btnH, EPD_BLACK, fb);
  // Center the text inside the button
  int32_t tx = btnX + 16, tby = btnY + btnH / 2 + th / 2;
  FontProperties props;
  props.fg_color = C_WHITE;
  props.bg_color = C_DKGRAY;
  props.flags = 0;
  props.fallback_glyph = 0;
  write_mode((GFXfont*)&MeltSwashes18, todayLabel, &tx, &tby, fb, BLACK_ON_WHITE, &props);
}

static void drawRightFooter(uint8_t* fb) {
  int rx, ry, rw, rh;
  getRightFooterRect(rx, ry, rw, rh);

  // Striped background.
  epd_fill_rect(rx + 1, ry, rw - 2, rh, EPD_LTGRAY, fb);
  for (int i = 0; i < rh; i += 2) {
    epd_draw_hline(rx + 1, ry + i, rw - 2, EPD_WHITE, fb);
  }
  epd_draw_rect(rx + 1, ry, rw - 2, rh, EPD_BLACK, fb);

  // Forward arrow (right side of the right footer)
  int arrowSize = 60;
  int arrowX = rx + rw - 12 - arrowSize;
  int arrowY = ry + (rh - arrowSize) / 2;
  drawArrowButton(arrowX, arrowY, arrowSize, arrowSize, false, fb);

  // Battery icon (left side of the right footer)
  int batW = 48, batH = 24;
  int batX = rx + 12;
  int batY = ry + (rh - batH) / 2;
  int batPercent = battery::lastPercent();
  if (batPercent < 0) batPercent = 0;
  drawBatteryIcon(batX, batY, batW, batH, batPercent, fb);

  // Gear icon (only in debug mode, between battery and arrow).
#ifndef ENV_DEMO
  if (s_debugMode) {
    int gearX = batX + batW + 20;
    int gearY = ry + (rh - 16) / 2;
    epd_draw_circle(gearX + 8, gearY + 8, 8, EPD_BLACK, fb);
    epd_fill_rect(gearX + 6, gearY - 2, 4, 4, EPD_BLACK, fb);
    epd_draw_circle(gearX + 8, gearY + 8, 3, EPD_BLACK, fb);
  }
#endif
}

// Lane assignment for an event in a column. `lane` is the index of the lane
// the event occupies (0-based); `laneCount` is the total number of lanes
// active during this event's lifetime (determines width).
struct LaneAssignment {
  int lane;       // 0-based lane index
  int laneCount;  // total lanes during this event (1 = full width, no overlap)
};

// Compute lane assignments for a day's events. `events` is the global event
// array, `indices` holds the event indices for this day (sorted by start time,
// earliest first), and `count` is its length. Writes one LaneAssignment per
// event into `outAssignments[]`.
//
// Algorithm: greedy coloring. For each event, find the first lane whose
// previous event has ended; reuse it, else start a new lane. After placing
// all events, walk them again and compute each event's lane count as the
// max number of lanes that overlap with that event's time range.
static void computeLaneAssignments(const CalendarEvent* events,
                                   const int* indices, int count,
                                   LaneAssignment* outAssignments) {
  if (count <= 0) return;

  // First pass: assign each event to a lane. All-day events are rendered as
  // a separate banner and should not occupy lanes in the timeline.
  // activeLaneEnds[i] = the end time (in minutes from midnight) of the event
  // currently in lane i, or -1 if the lane is free.
  int activeLaneEnds[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
  int laneAssigned[32];  // event index → lane index
  for (int e = 0; e < count; e++) {
    if (events[indices[e]].allDay) {
      laneAssigned[e] = 0;
      continue;
    }

    int startMin = events[indices[e]].startHour * 60 + events[indices[e]].startMin;
    int endMin = startMin + events[indices[e]].durationMin;

    // Find first free lane (whose event has ended before this one starts).
    int chosen = -1;
    for (int l = 0; l < 8; l++) {
      if (activeLaneEnds[l] <= startMin) {
        chosen = l;
        break;
      }
    }
    if (chosen < 0) {
      // Out of lanes — cap at 8. Shouldn't happen with realistic calendars.
      chosen = 7;
    }
    laneAssigned[e] = chosen;
    activeLaneEnds[chosen] = endMin;
  }

  // Second pass: compute max overlap during each event's lifetime.
  for (int e = 0; e < count; e++) {
    if (events[indices[e]].allDay) {
      outAssignments[e].lane = 0;
      outAssignments[e].laneCount = 1;
      continue;
    }

    int startMin = events[indices[e]].startHour * 60 + events[indices[e]].startMin;
    int endMin = startMin + events[indices[e]].durationMin;

    int maxLanes = 1;
    for (int t = startMin; t < endMin; t += 5) {  // sample every 5 minutes
      int active = 0;
      for (int e2 = 0; e2 < count; e2++) {
        if (events[indices[e2]].allDay) continue;
        int s2 = events[indices[e2]].startHour * 60 + events[indices[e2]].startMin;
        int e2end = s2 + events[indices[e2]].durationMin;
        if (s2 <= t && t < e2end) active++;
      }
      if (active > maxLanes) maxLanes = active;
    }
    outAssignments[e].lane = laneAssigned[e];
    outAssignments[e].laneCount = maxLanes;
  }
}

// ---------------------------------------------------------------------------
// Demo 1: focus+context weekly view (3 days)
// ---------------------------------------------------------------------------
static void renderWeeklyView() {
  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);

  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);

  constexpr int numCols = 3;  // Phase 10: focus + 2 context columns
  const int dayStartHour = resolvedDayStartHour();
  const int dayEndHour   = resolvedDayEndHour();

  constexpr int COL_GAP = 8;
  // 3 columns: 2 context + 1 focus (focus = 2 × context).
  // Total width = 4 context units + 2 gaps → context_w = (cw - 2*gap) / 4
  int contextW = (cw - COL_GAP * 2) / 4;
  int focusW = contextW * 2;

  const int dayMinutes = (dayEndHour - dayStartHour) * 60;

  struct tm base;
  getDemoBaseDay(base);
  base.tm_mday += s_baseDayOffset;
  mktime(&base);

  int headerH = 56;
  int timelineTop = cy + headerH + 4;
  // Shared timeline geometry — same across all columns so grid lines align.
  int timelineH = ch - headerH - 8;  // 8 = 4 px top gap + 4 px bottom padding

  for (int i = 0; i < numCols; i++) {
    struct tm day = base;
    day.tm_mday += (i - 1);  // i=0 → focus-1, i=1 → focus, i=2 → focus+1
    mktime(&day);

    // Per-column x and width: context, focus, context
    int x, colW;
    if (i == 0) {
      x = cx;
      colW = contextW;
    } else if (i == 1) {
      x = cx + contextW + COL_GAP;
      colW = focusW;
    } else {
      x = cx + contextW + COL_GAP + focusW + COL_GAP;
      colW = contextW;
    }

    bool isFocus = (i == 1);
    bool isToday = isTodayColumn(i);  // today's offset is 0

    // Focus column extends to the bottom of the screen; context columns stop
    // above the split footer. The timeline itself uses the same height everywhere
    // so grid lines align horizontally; focus just has empty space below it.
    int effectiveCh = isFocus ? ((EPD_HEIGHT - 4) - cy) : ch;

    epd_draw_rect(x, cy, colW, effectiveCh, EPD_BLACK, fb);

    const char* nameStr = isFocus ? dayName(day.tm_wday) : dayNameShort(day.tm_wday);
    char headerStr[32];
    snprintf(headerStr, sizeof(headerStr), "%s %02d/%02d",
             nameStr, day.tm_mon + 1, day.tm_mday);

    // Subtle background tint behind the day-of-week header for today's column.
    if (isToday) {
      epd_fill_rect(x, cy, colW, 56, EPD_LTGRAY, fb);  // 56 px tall band matching the header area
    }

    drawCenteredText(headerStr, x + colW / 2, cy + 42, (GFXfont*)&Genty24, fb);

    char dateStr[12];
    dateToString(day, dateStr, sizeof(dateStr));
    int colIndices[32];
    int colCount = eventsForDateLogged(dateStr, colIndices, 32);

    // If this day has no events, render a centered "No events" placeholder and
    // skip the rest of the column drawing (timeline, events, all-day banner).
    if (colCount == 0) {
      const char* msg = "No events";
      // Center horizontally in the column, vertically in the timeline area.
      int centerY = timelineTop + timelineH / 2;
      int centerX = x + colW / 2;
      // Render in MeltSwashes18, gray-on-white (subtle).
      int32_t tw = 0, th = 0, tx1 = 0, ty1 = 0;
      int32_t mcx = centerX, mcy = centerY;
      get_text_bounds((GFXfont*)&MeltSwashes18, msg, &mcx, &mcy, &tx1, &ty1, &tw, &th, NULL);
      int32_t tx = centerX - tw / 2;
      int32_t ty = centerY + th / 2;
      FontProperties props;
      props.fg_color = C_LTGRAY;  // gray, deliberately understated
      props.bg_color = C_WHITE;
      props.flags = 0;
      props.fallback_glyph = 0;
      write_mode((GFXfont*)&MeltSwashes18, msg, &tx, &ty, fb, BLACK_ON_WHITE, &props);
      continue;  // skip the rest of the loop body for this column
    }

    // ----- All-day banner titles (collected here, drawn after grid lines) -----
    const char* allDayTitles[8];
    int allDayTitleCount = 0;
    for (int e = 0; e < colCount; e++) {
      const CalendarEvent& ev = s_events[colIndices[e]];
      if (!ev.allDay) continue;
      bool already = false;
      for (int t = 0; t < allDayTitleCount; t++) {
        if (strcmp(allDayTitles[t], ev.title) == 0) { already = true; break; }
      }
      if (!already && allDayTitleCount < 8) {
        allDayTitles[allDayTitleCount++] = ev.title;
      }
    }

    // Count events entirely outside the visible time range so we can hint at
    // the top/bottom of the timeline when early-morning or late-evening events
    // are hidden.
    int beforeRangeCount = 0;
    int afterRangeCount = 0;
    for (int e = 0; e < colCount; e++) {
      const CalendarEvent& ev = s_events[colIndices[e]];
      if (ev.allDay) continue;
      int startMin = ev.startHour * 60 + ev.startMin;
      int endMin = startMin + ev.durationMin;
      if (endMin <= dayStartHour * 60) beforeRangeCount++;
      else if (startMin >= dayEndHour * 60) afterRangeCount++;
    }

    // 3-hour grid markers (same Y in every column).
    for (int h = dayStartHour; h <= dayEndHour; h += 3) {
      int minFromStart = (h - dayStartHour) * 60;
      int yy = timelineTop + (minFromStart * timelineH) / dayMinutes;
      epd_draw_hline(x + 8, yy, colW - 16, EPD_LTGRAY, fb);
    }

    // Subtle terminator at the bottom of the timeline area.
    int timelineBottomY = timelineTop + timelineH;
    epd_draw_hline(x + 4, timelineBottomY, colW - 8, EPD_LTGRAY, fb);

    // Draw the all-day banner as a black rectangle overlaid at the top of
    // the timeline area. Grid lines and any 7 AM event behind it are hidden;
    // that is acceptable because the banner is more important.
    if (allDayTitleCount > 0) {
      // Compute prev-day and next-day date strings (for multi-day arrow detection).
      struct tm prevDay = day;
      prevDay.tm_mday -= 1;
      mktime(&prevDay);
      char prevDateStr[12];
      dateToString(prevDay, prevDateStr, sizeof(prevDateStr));

      struct tm nextDay = day;
      nextDay.tm_mday += 1;
      mktime(&nextDay);
      char nextDateStr[12];
      dateToString(nextDay, nextDateStr, sizeof(nextDateStr));

      // Build the banner text with multi-day arrows per title.
      char bannerText[96] = "";
      for (int t = 0; t < allDayTitleCount; t++) {
        const char* title = allDayTitles[t];
        bool continuedFromPrev = hasAllDayEventOnDate(prevDateStr, title);
        bool continuesToNext   = hasAllDayEventOnDate(nextDateStr, title);

        // Build "← Title →" or subset
        char entry[80];
        snprintf(entry, sizeof(entry), "%s%s%s",
                 continuedFromPrev ? "← " : "",
                 title,
                 continuesToNext ? " →" : "");

        // Append to bannerText with ", " separator if not first
        if (t > 0) {
          strlcat(bannerText, ", ", sizeof(bannerText));
        }
        strlcat(bannerText, entry, sizeof(bannerText));
      }

      int bannerRectH = 36;
      int bannerX = x + 4;
      int bannerY = timelineTop;
      int bannerW = colW - 8;

      epd_fill_rect(bannerX, bannerY, bannerW, bannerRectH, EPD_BLACK, fb);

      // Truncate the combined text to fit, then center it inside the banner rect.
      char fitText[96];
      truncateToFitWidth((GFXfont*)&MeltSwashes18, bannerText, bannerW - 16,
                         fitText, sizeof(fitText));
      drawCenteredTextColored(fitText, x + colW / 2, bannerY + bannerRectH / 2 + 6,
                              (GFXfont*)&MeltSwashes18, fb, C_WHITE, C_BLACK);
    }

    // Indicator for events before the visible time range (early morning).
    int beforeIndicatorY = timelineTop + 12;
    if (allDayTitleCount > 0) {
      beforeIndicatorY = timelineTop + 36 + 4;  // render below the banner
    }
    if (beforeRangeCount > 0) {
      char indBuf[32];
      snprintf(indBuf, sizeof(indBuf), "↑ %d before %dAM", beforeRangeCount, dayStartHour);
      FontProperties props;
      props.fg_color = C_DKGRAY;
      props.bg_color = C_WHITE;
      props.flags = 0;
      props.fallback_glyph = 0;
      int32_t ix = x + 8, iy = beforeIndicatorY;
      write_mode((GFXfont*)&MeltSwashes14, indBuf, &ix, &iy, fb, BLACK_ON_WHITE, &props);
    }

    // Indicator for events after the visible time range (late evening).
    if (afterRangeCount > 0) {
      char indBuf[32];
      snprintf(indBuf, sizeof(indBuf), "↓ %d after %dPM", afterRangeCount, dayEndHour - 12);
      FontProperties props;
      props.fg_color = C_DKGRAY;
      props.bg_color = C_WHITE;
      props.flags = 0;
      props.fallback_glyph = 0;
      int32_t ix = x + 8, iy = timelineTop + timelineH - 4;
      write_mode((GFXfont*)&MeltSwashes14, indBuf, &ix, &iy, fb, BLACK_ON_WHITE, &props);
    }

    // Sort by start time, earliest first. All-day events sort before timed ones.
    for (int i = 1; i < colCount; i++) {
      int idx = colIndices[i];
      int keyMin = s_events[idx].allDay ? -1
                                        : s_events[idx].startHour * 60 + s_events[idx].startMin;
      int j = i - 1;
      while (j >= 0) {
        int prevMin = s_events[colIndices[j]].allDay ? -1
                                                     : s_events[colIndices[j]].startHour * 60
                                                           + s_events[colIndices[j]].startMin;
        if (prevMin > keyMin) {
          colIndices[j + 1] = colIndices[j];
          j--;
        } else {
          break;
        }
      }
      colIndices[j + 1] = idx;
    }

    // Compute lane assignments for this day's events (focus column only).
    LaneAssignment laneAssignments[32];
    if (isFocus) {
      computeLaneAssignments(s_events, colIndices, colCount, laneAssignments);
    } else {
      // Context columns: every event in 1-lane mode (full column width).
      for (int e = 0; e < colCount; e++) {
        laneAssignments[e].lane = 0;
        laneAssignments[e].laneCount = 1;
      }
    }

    // Draw gap indicators between events that have a > 60 minute gap between
    // them. Only consider events in lane 0 (the lane that defines the timeline's
    // visual flow). Drawn before the event blocks so events render on top.
    {
      int prevEndMin = -1;
      for (int e = 0; e < colCount; e++) {
        const CalendarEvent& ev = s_events[colIndices[e]];
        if (ev.allDay) continue;

        int startMin = ev.startHour * 60 + ev.startMin;
        if (prevEndMin > 0 && (startMin - prevEndMin) >= 60) {
          // Draw a gap indicator centered between prevEndY and this event's blockY
          int prevEndY = timelineTop
                         + ((prevEndMin - dayStartHour * 60) * timelineH) / dayMinutes;
          int thisStartY = timelineTop
                           + ((startMin - dayStartHour * 60) * timelineH) / dayMinutes;
          int midY = (prevEndY + thisStartY) / 2;
          drawGapIndicator(x + 8, midY, colW - 16, EPD_LTGRAY, fb);
        }

        int endMin = startMin + ev.durationMin;
        if (endMin > prevEndMin) prevEndMin = endMin;
      }
    }

    for (int e = 0; e < colCount; e++) {
      const CalendarEvent& ev = s_events[colIndices[e]];

      if (ev.allDay) continue;  // all-day events handled in the banner above


      int startMin = ev.startHour * 60 + ev.startMin;
      int endMin   = startMin + ev.durationMin;
      int visStart = max(startMin, dayStartHour * 60);
      int visEnd   = min(endMin, dayEndHour * 60);
      int visDur   = visEnd - visStart;
      if (visDur <= 0) continue;

      int blockY = timelineTop + ((visStart - dayStartHour * 60) * timelineH) / dayMinutes;
      int blockH = computeBlockHeight(visDur, timelineH, dayMinutes);
      int timelineBottom = timelineTop + timelineH;
      if (blockY + blockH > timelineBottom - 4) blockH = timelineBottom - 4 - blockY;

      // Render slightly shorter than the computed height to create a visual gap
      // between adjacent events. EVENT_GAP is applied to both 1-line and 2-line
      // event blocks. Text positions are anchored from the top (blockY + offset),
      // so they stay correct.
      int renderH = blockH - EVENT_GAP;
      if (renderH < 1) renderH = 1;

      // Default: full column width (no lane splitting).
      int laneW = colW - 12;
      int laneX = x + 6;

      // For the focus column, narrow the block to its lane width if multiple lanes.
      if (isFocus) {
        const LaneAssignment& la = laneAssignments[e];
        if (la.laneCount > 1) {
          int totalGap = (la.laneCount - 1) * 4;  // 4 px gap between lanes
          laneW = (colW - 12 - totalGap) / la.laneCount;
          laneX = x + 6 + la.lane * (laneW + 4);
        }
      }

      int blockX = laneX;
      int blockW = laneW;

      epd_fill_rect(blockX, blockY, blockW, renderH, ev.shade << 4, fb);
      epd_draw_rect(blockX, blockY, blockW, renderH, EPD_BLACK, fb);

      FontProperties props;
      props.fg_color = (ev.shade <= C_MDGRAY) ? C_WHITE : C_BLACK;
      props.bg_color = ev.shade;
      props.flags = 0;
      props.fallback_glyph = 0;

      if (ev.durationMin <= SHORT_EVENT_THRESHOLD) {
        // ----- 1-line short-event rendering -----
        char timeStr[16];
        formatTimeCompact(ev.startHour, ev.startMin, timeStr, sizeof(timeStr));

        // Build "Time Title" combined string
        char combined[96];
        snprintf(combined, sizeof(combined), "%s %s", timeStr, ev.title);

        // Truncate to fit block width (leave 8 px left padding + 8 px right padding)
        char lineBuf[80];
        truncateToFitWidth((GFXfont*)&MeltSwashes16, combined, blockW - 16,
                           lineBuf, sizeof(lineBuf));

        // Vertically center the line in the block
        // MeltSwashes16 ascender ~10, descender ~3 -> text height ~13
        // Baseline at blockY + (renderH + 10) / 2 approximately centers it
        int centerY = blockY + (renderH + 10) / 2;

        if (props.fg_color == C_WHITE) {
          drawTextWithOutline((GFXfont*)&MeltSwashes16, lineBuf,
                              blockX + 8, centerY, fb,
                              C_WHITE, props.bg_color);
        } else {
          drawTextColored((GFXfont*)&MeltSwashes16, lineBuf,
                          blockX + 8, centerY, fb,
                          props.fg_color, props.bg_color);
        }
        continue;  // skip the 2-line rendering below
      }

      // ----- existing 2-line rendering for events > 60 min -----
      char timeStr[16];
      formatTime(ev.startHour, ev.startMin, timeStr, sizeof(timeStr));

      // Time
      if (props.fg_color == C_WHITE) {
        drawTextWithOutline((GFXfont*)&MeltSwashes18, timeStr,
                            blockX + 8, blockY + 16, fb,
                            C_WHITE, props.bg_color);
      } else {
        int32_t ttcx = blockX + 8, ttcy = blockY + 16;
        write_mode((GFXfont*)&MeltSwashes18, timeStr, &ttcx, &ttcy, fb,
                   BLACK_ON_WHITE, &props);
      }

      // Compute available height for text below the time line.
      // Time baseline is at blockY + 16; text below starts at blockY + 50.
      const int TITLE_TOP_Y = 50;
      const int TITLE_LINE_HEIGHT = 22;
      const int LOC_LINE_HEIGHT = 20;
      const int BOTTOM_PADDING = 4;

      int availH = renderH - TITLE_TOP_Y - BOTTOM_PADDING;
      int maxTitleLines = 1;
      if (availH >= 2 * TITLE_LINE_HEIGHT) maxTitleLines = 2;

      bool showLocation = false;
      if (availH >= maxTitleLines * TITLE_LINE_HEIGHT + LOC_LINE_HEIGHT
          && ev.location && ev.location[0]) {
        showLocation = true;
      }

      // Wrap title to the available line budget.
      WrappedText wrapped = wrapText((GFXfont*)&MeltSwashes16, ev.title,
                                      blockW - 16, maxTitleLines);

      // Title lines
      for (int li = 0; li < wrapped.count; li++) {
        int lineY = blockY + TITLE_TOP_Y + li * TITLE_LINE_HEIGHT;
        if (lineY > blockY + renderH - BOTTOM_PADDING) break;  // safety
        if (props.fg_color == C_WHITE) {
          drawTextWithOutline((GFXfont*)&MeltSwashes16, wrapped.lines[li],
                              blockX + 8, lineY, fb,
                              C_WHITE, props.bg_color);
        } else {
          drawTextColored((GFXfont*)&MeltSwashes16, wrapped.lines[li],
                          blockX + 8, lineY, fb,
                          props.fg_color, props.bg_color);
        }
      }

      // Optional location line below the title.
      if (showLocation) {
        int locY = blockY + TITLE_TOP_Y + wrapped.count * TITLE_LINE_HEIGHT + 2;
        char locBuf[48];
        truncateToFitWidth((GFXfont*)&MeltSwashes14, ev.location, blockW - 16,
                           locBuf, sizeof(locBuf));
        drawTextColored((GFXfont*)&MeltSwashes14, locBuf,
                        blockX + 8, locY, fb,
                        props.fg_color, props.bg_color);
      }

      // End time at the bottom of the block — only if there's room and no
      // overlapping event would visually collide with it.
      const int END_TIME_MIN_BLOCK_H = 100;   // need this much height for title + end time
      const int END_TIME_BOTTOM_PADDING = 12;  // baseline is this far from block bottom

      // In the focus column, lane splitting places overlapping events
      // side-by-side, so a follower in another lane doesn't cover this block's
      // end-time area. Suppress only when an overlapping event would paint on
      // top (context columns or single-lane focus).
      bool suppressEndTime = hasOverlappingFollower(ev);
      if (isFocus && laneAssignments[e].laneCount > 1) {
        suppressEndTime = false;
      }

      if (ev.durationMin > SHORT_EVENT_THRESHOLD
          && blockH >= END_TIME_MIN_BLOCK_H
          && !suppressEndTime) {
        // Compute end time
        int endTotal = ev.startHour * 60 + ev.startMin + ev.durationMin;
        int eh = (endTotal / 60) % 24;
        int emin = endTotal % 60;
        char endStr[16];
        formatTime(eh, emin, endStr, sizeof(endStr));

        int endY = blockY + renderH - END_TIME_BOTTOM_PADDING;
        if (props.fg_color == C_WHITE) {
          drawTextWithOutline((GFXfont*)&MeltSwashes18, endStr,
                              blockX + 8, endY, fb,
                              C_WHITE, props.bg_color);
        } else {
          int32_t ex = blockX + 8, ey = endY;
          write_mode((GFXfont*)&MeltSwashes18, endStr, &ex, &ey, fb,
                     BLACK_ON_WHITE, &props);
        }
      }
    }
  }

  drawLeftFooter(fb);
  drawRightFooter(fb);
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
  int count = eventsForDateLogged(dateStr, indices, 32);
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
    write_mode((GFXfont*)&MeltSwashes18, statusBuf, &sx, &sy, fb, BLACK_ON_WHITE, &sp);
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
    // Center "< Back to Week" horizontally within the wider button.
    const char* backStr = "< Back to Week";
    int32_t tw = measureTextWidth((GFXfont*)&MeltSwashes18, backStr);
    int32_t btx = backX + (DAILY_BACK_W - tw) / 2;
    int32_t bty = backY + 34;  // baseline bumped 2px for visual centering
    writeln((GFXfont*)&MeltSwashes18, backStr, &btx, &bty, fb);
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
      writeln((GFXfont*)&MeltSwashes18, moreBuf, &mx, &my, fb);
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
      // All-day: "All day" label on top, title below in Genty24
      drawTextColored((GFXfont*)&MeltSwashes18, "All day",
                      listX + 12, listY + 28, fb, C_BLACK, rowBg);

      char lineBuf[80];
      snprintf(lineBuf, sizeof(lineBuf), "%s | %s", ev.title, ev.location);
      char outBuf[64];
      truncateToFitWidth((GFXfont*)&Genty24, lineBuf, listW - 24,
                         outBuf, sizeof(outBuf));
      int32_t titleX = listX + 12, titleY = listY + 60;
      write_mode((GFXfont*)&Genty24, outBuf, &titleX, &titleY, fb, BLACK_ON_WHITE, &rowProps);

      char descBuf[64];
      truncateToFitWidth((GFXfont*)&MeltSwashes18, ev.description, listW - 24,
                         descBuf, sizeof(descBuf));
      drawTextColored((GFXfont*)&MeltSwashes18, descBuf,
                      listX + 12, listY + 88, fb, C_BLACK, rowBg);
    } else {
      // Timed: time range on top in MeltSwashes20, title below in Genty24
      // NOTE: weekly view reverted to start-only; daily view kept as range for now.
      char timeStr[24];
      formatTimeRange(ev.startHour, ev.startMin, ev.durationMin, timeStr, sizeof(timeStr));

      drawTextColored((GFXfont*)&MeltSwashes20, timeStr,
                      listX + 12, listY + 30, fb, C_BLACK, rowBg);

      char lineBuf[80];
      snprintf(lineBuf, sizeof(lineBuf), "%s | %s", ev.title, ev.location);
      char outBuf[64];
      truncateToFitWidth((GFXfont*)&Genty24, lineBuf, listW - 24,
                         outBuf, sizeof(outBuf));
      int32_t titleX = listX + 12, titleY = listY + 62;
      write_mode((GFXfont*)&Genty24, outBuf, &titleX, &titleY, fb, BLACK_ON_WHITE, &rowProps);

      char descBuf[64];
      truncateToFitWidth((GFXfont*)&MeltSwashes18, ev.description, listW - 24,
                         descBuf, sizeof(descBuf));
      drawTextColored((GFXfont*)&MeltSwashes18, descBuf,
                      listX + 12, listY + 90, fb, C_BLACK, rowBg);
    }

    listY += lineH;
    displayed++;
  }

  drawLeftFooter(fb);
  drawRightFooter(fb);
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
  s_screen = (s_screen == SCREEN_WEEKLY) ? SCREEN_DAILY : SCREEN_WEEKLY;
}

} // namespace ui
