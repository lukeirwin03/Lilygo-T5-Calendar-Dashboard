#include "ui.h"
#include "dashboards/calendar_dashboard.h"
#include "display_manager.h"
#include "epd_driver.h"
#include "fonts/Genty24pt7b.h"
#include "fonts/Genty32pt7b.h"
#include "fonts/MeltSwashes14pt7b.h"
#include "fonts/MeltSwashes16pt7b.h"
#include "fonts/MeltSwashes18pt7b.h"
#include "ui_settings.h"
#include "settings.h"
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

static constexpr int EVENT_GAP = 4;  // px of white space between adjacent event blocks

static constexpr int MIN_BLOCK_HEIGHT = 76;  // px; tuned for MeltSwashes18 time + MeltSwashes16 title

static constexpr int SHORT_EVENT_THRESHOLD = 60;   // min; events <= this use 1-line format
static constexpr int SHORT_EVENT_MIN_H     = 28;   // px; 1-line block min height
static constexpr int SHORT_EVENT_MAX_H     = 50;   // px; 1-line block max height (at 60 min)
static constexpr int LONG_EVENT_CAP_MIN    = 480;  // min; events longer than this (8hr) plateau — likely all-day anyway

// Daily view layout — shared between renderDailyView() and the daily nav
// hit-tests to prevent coordinate mismatches.
static constexpr int DAILY_PAGE_SIZE    = 220;  // tear-off calendar square (was 280)
static constexpr int DAILY_PAGE_MARGIN  = 16;   // offset from content rect
static constexpr int DAILY_BACK_GAP     = 24;   // gap between calendar and nav container
static constexpr int DAILY_LIST_GAP     = 24;   // gap between calendar column and list
static constexpr int DAILY_ROW_H        = 60;   // compact 2-line row (was 100)
static constexpr int DAILY_STATUS_H     = 32;   // status strip height

static constexpr int DAILY_NAV_W        = 220;  // container width (matches calendar & old back button)
static constexpr int DAILY_NAV_BACK_H   = 64;   // "Back to Week" row height
static constexpr int DAILY_NAV_ARROW_H  = 64;   // prev/next arrow row height

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

// Daily view: selected event for detail display. -1 = list mode, >= 0 = detail mode
// storing the global event index into s_events[].
static int s_selectedEventIdx = -1;

// Cached sorted event indices for the current daily view, used by touch hit-testing.
static int s_dailyIndices[32];
static int s_dailyCount = 0;

static bool       s_pendingRender = false;

// Refresh mode: full or partial (settings/daily row update).
enum RefreshMode { REFRESH_FULL, REFRESH_PARTIAL_SETTINGS, REFRESH_PARTIAL_DAILY };
static RefreshMode s_refreshMode = REFRESH_FULL;

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

// Release gate: after a gesture is processed, require at least one !isTouched
// poll before accepting new touches. Prevents stale touch data buffered during
// the multi-second e-paper render from being misinterpreted as a second tap.
static bool          s_awaitingRelease = false;
static unsigned long s_awaitingReleaseMs = 0;

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

// Resolve the timeline bounds from the persisted user settings.
static int resolvedDayStartHour() {
  return settings::get().day_start_hour;
}
static int resolvedDayEndHour() {
  return settings::get().day_end_hour;
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
  hour = hour % 24;
  if (settings::get().time_format_24h) {
    snprintf(buf, len, "%02d:%02d", hour, min);
    return;
  }
  int h = hour % 12;
  if (h == 0) h = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  snprintf(buf, len, "%d:%02d %s", h, min, ampm);
}

// Compact time format: "3:30PM" (no space before AM/PM). Used in the
// 1-line short-event format where horizontal space is at a premium.
static void formatTimeCompact(int hour, int min, char* buf, size_t len) {
  hour = hour % 24;
  if (settings::get().time_format_24h) {
    snprintf(buf, len, "%02d:%02d", hour, min);
    return;
  }
  int h = hour % 12;
  if (h == 0) h = 12;
  const char* ampm = (hour < 12) ? "AM" : "PM";
  snprintf(buf, len, "%d:%02d%s", h, min, ampm);
}

// Ultra-compact 12-hour range. Same half: "9:00-10:00AM".
// Different halves: "11:30A-1:00P". For narrow context columns.
static void formatTimeRangeUltraCompact(int startHour, int startMin, int durationMin,
                                         char* buf, size_t len) {
  int endTotal = startHour * 60 + startMin + durationMin;
  int eh = (endTotal / 60) % 24;
  int em = endTotal % 60;
  if (settings::get().time_format_24h) {
    snprintf(buf, len, "%02d:%02d-%02d:%02d", startHour % 24, startMin, eh, em);
    return;
  }
  int sh12 = startHour % 12;  if (sh12 == 0) sh12 = 12;
  int eh12 = eh % 12;          if (eh12 == 0) eh12 = 12;
  const char* sap = (startHour < 12) ? "AM" : "PM";
  const char* eap = (eh < 12) ? "AM" : "PM";

  if (sap == eap) {
    // Same half — show AM/PM once at the end: "9:00-10:00AM"
    snprintf(buf, len, "%d:%02d-%d:%02d%s", sh12, startMin, eh12, em, eap);
  } else {
    // Different halves — abbreviate each: "11:30A-1:00P"
    snprintf(buf, len, "%d:%02d%c-%d:%02d%c", sh12, startMin, sap[0], eh12, em, eap[0]);
  }
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
  if (settings::get().time_format_24h) {
    snprintf(buf, len, "%02d:%02d - %02d:%02d", startHour % 24, startMin, eh, em);
    return;
  }

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
//   the proportional value capped at LONG_EVENT_CAP_MIN (8 hr) so very long events
//   plateau rather than dominating the column.
static int computeBlockHeight(int durationMin, int timelineH, int dayMinutes, int minH) {
  if (durationMin <= SHORT_EVENT_THRESHOLD) {
    int clamped = max(30, durationMin);
    return SHORT_EVENT_MIN_H
           + (clamped - 30) * (SHORT_EVENT_MAX_H - SHORT_EVENT_MIN_H) / 30;
  }
  int cappedDur = min(durationMin, LONG_EVENT_CAP_MIN);
  int proportional = (cappedDur * timelineH) / dayMinutes;
  return max(minH, proportional);
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

// Draw the all-day event banner as a black rectangle at the top of the timeline.
// `allDayTitles` is an array of title strings, `allDayTitleCount` is its length.
// `day` is the current day struct (for computing prev/next-day multi-day arrows).
// Does nothing if allDayTitleCount == 0.
static void drawAllDayBanner(const char** allDayTitles, int allDayTitleCount,
                             const struct tm& day,
                             int x, int colW, int timelineTop,
                             uint8_t* fb) {
  if (allDayTitleCount == 0) return;

  // Compute prev-day and next-day date strings for multi-day arrow detection.
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

  char bannerText[96] = "";
  for (int t = 0; t < allDayTitleCount; t++) {
    const char* title = allDayTitles[t];
    bool continuedFromPrev = hasAllDayEventOnDate(prevDateStr, title);
    bool continuesToNext   = hasAllDayEventOnDate(nextDateStr, title);

    char entry[80];
    snprintf(entry, sizeof(entry), "%s%s%s",
             continuedFromPrev ? "← " : "",
             title,
             continuesToNext ? " →" : "");

    if (t > 0) strlcat(bannerText, ", ", sizeof(bannerText));
    strlcat(bannerText, entry, sizeof(bannerText));
  }

  int bannerRectH = 36;
  int bannerX = x + 4;
  int bannerY = timelineTop;
  int bannerW = colW - 8;

  epd_fill_rect(bannerX, bannerY, bannerW, bannerRectH, EPD_BLACK, fb);

  char fitText[96];
  truncateToFitWidth((GFXfont*)&MeltSwashes18, bannerText, bannerW - 16,
                     fitText, sizeof(fitText));
  drawCenteredTextColored(fitText, x + colW / 2, bannerY + bannerRectH / 2 + 6,
                          (GFXfont*)&MeltSwashes18, fb, C_WHITE, C_BLACK);
}

// Render a chronological text list of timed events for a context column.
// Each event occupies 2 lines: ultra-compact time range (MeltSwashes14, DKGRAY)
// on the first line, truncated title (MeltSwashes14, BLACK) on the second.
// All-day events are skipped (they render in the banner). Events are listed
// top-to-bottom in the order they appear in colIndices (sorted by start time).
// Shows "+N more..." at the bottom if not all events fit.
static void drawContextList(const int* colIndices, int count,
                            int x, int colW,
                            int listStartY, int listEndY,
                            uint8_t* fb) {
  const int TIME_BASELINE = 22;    // baseline of time text (MeltSwashes14 ascender is ~22)
  const int TITLE_BASELINE = 48;   // baseline of title text (26px below time, clears descender)
  const int ENTRY_HEIGHT = 60;     // total height per entry

  const int PADDING = 8;
  int textW = colW - PADDING * 2;

  int entryTop = listStartY;
  int displayed = 0;

  for (int e = 0; e < count; e++) {
    const CalendarEvent& ev = s_events[colIndices[e]];
    if (ev.allDay) continue;

    // Need room for both the time line and the title line.
    if (entryTop + TITLE_BASELINE > listEndY) {
      // Show "+N more..." for remaining timed events.
      // Use the row background that would alternate next.
      int remaining = 0;
      for (int e2 = e; e2 < count; e2++) {
        if (!s_events[colIndices[e2]].allDay) remaining++;
      }
      if (remaining > 0) {
        uint8_t rowBg = (displayed % 2 == 1) ? C_LTGRAY : C_WHITE;
        if (rowBg == C_LTGRAY) {
          epd_fill_rect(x + 1, entryTop, colW - 2, ENTRY_HEIGHT, EPD_LTGRAY, fb);
        }
        char moreStr[24];
        snprintf(moreStr, sizeof(moreStr), "+%d more...", remaining);
        drawTextColored((GFXfont*)&MeltSwashes14, moreStr,
                        x + PADDING, entryTop + TIME_BASELINE, fb,
                        C_DKGRAY, rowBg);
      }
      break;
    }

    // Alternating row background for odd entries (zebra stripes).
    uint8_t rowBg = (displayed % 2 == 1) ? C_LTGRAY : C_WHITE;
    if (rowBg == C_LTGRAY) {
      epd_fill_rect(x + 1, entryTop, colW - 2, ENTRY_HEIGHT, EPD_LTGRAY, fb);
    }

    // Line 1: ultra-compact time range (e.g., "9:00A-10:00A")
    char timeBuf[20];
    formatTimeRangeUltraCompact(ev.startHour, ev.startMin, ev.durationMin,
                                timeBuf, sizeof(timeBuf));
    char timeFit[20];
    truncateToFitWidth((GFXfont*)&MeltSwashes14, timeBuf, textW,
                       timeFit, sizeof(timeFit));
    drawTextColored((GFXfont*)&MeltSwashes14, timeFit,
                    x + PADDING, entryTop + TIME_BASELINE, fb,
                    C_DKGRAY, rowBg);

    // Line 2: title (truncated to fit column width)
    char titleBuf[64];
    truncateToFitWidth((GFXfont*)&MeltSwashes14, ev.title, textW,
                       titleBuf, sizeof(titleBuf));
    drawTextColored((GFXfont*)&MeltSwashes14, titleBuf,
                    x + PADDING, entryTop + TITLE_BASELINE, fb,
                    C_BLACK, rowBg);

    entryTop += ENTRY_HEIGHT;
    displayed++;
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
static void drawArrowButton(int x, int y, int w, int h, bool left, uint8_t* fb) {

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
  int bindingH = 28;
  int tearH = 12;

  epd_fill_rect(x, y, size, size, EPD_WHITE, fb);
  epd_draw_rect(x, y, size, size, EPD_BLACK, fb);

  // Binding strip at top
  epd_fill_rect(x, y, size, bindingH, EPD_DKGRAY, fb);
  epd_draw_rect(x, y, size, bindingH, EPD_BLACK, fb);

  // Rings
  for (int rx = x + 16; rx < x + size; rx += 34) {
    epd_fill_rect(rx, y + 7, 12, 12, EPD_WHITE, fb);
    epd_draw_rect(rx, y + 7, 12, 12, EPD_BLACK, fb);
  }

  // Zigzag tear line just below binding
  int tearY = y + bindingH + 1;
  int zigW = 16;
  for (int zx = x; zx < x + size - 1; zx += zigW) {
    epd_draw_line(zx, tearY, zx + zigW / 2, tearY + tearH, EPD_BLACK, fb);
    epd_draw_line(zx + zigW / 2, tearY + tearH, zx + zigW, tearY, EPD_BLACK, fb);
  }

  // Day name (Genty24)
  int nameY = y + bindingH + tearH + 36;
  drawCenteredText(dayName(day.tm_wday), x + size / 2, nameY,
                   (GFXfont*)&Genty24, fb);

  // Large day number (Genty32)
  char numStr[8];
  snprintf(numStr, sizeof(numStr), "%d", day.tm_mday);
  int numberY = nameY + 80;
  drawCenteredText(numStr, x + size / 2, numberY, (GFXfont*)&Genty32, fb);
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
  s_selectedEventIdx = -1;
  s_pendingRender = true;
  s_refreshMode = REFRESH_FULL;
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
}

static void triggerScreenChange(Screen next, int dayOffset) {
  s_screen = next;
  s_baseDayOffset = dayOffset;
  s_pendingRender = true;
  s_refreshMode = REFRESH_FULL;
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
}

// Container sits in the left column, directly below the tear-off calendar.
static void getDailyNavRect(int& nx, int& ny, int& nw, int& nh) {
  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);
  int contentTop = cy + DAILY_STATUS_H + 8;
  int pageX = cx + DAILY_PAGE_MARGIN;
  int pageY = contentTop;
  nx = pageX;
  ny = pageY + DAILY_PAGE_SIZE + DAILY_BACK_GAP;
  nw = DAILY_NAV_W;
  nh = DAILY_NAV_BACK_H + DAILY_NAV_ARROW_H;
}

static bool hitDailyBackToWeek(int16_t x, int16_t y) {
  if (s_screen != SCREEN_DAILY) return false;
  int nx, ny, nw, nh;
  getDailyNavRect(nx, ny, nw, nh);
  return (x >= nx && x <= nx + nw && y >= ny && y <= ny + DAILY_NAV_BACK_H);
}
static bool hitDailyPrev(int16_t x, int16_t y) {
  if (s_screen != SCREEN_DAILY) return false;
  int nx, ny, nw, nh;
  getDailyNavRect(nx, ny, nw, nh);
  int arrowTop = ny + DAILY_NAV_BACK_H;
  return (x >= nx && x < nx + nw / 2 && y >= arrowTop && y <= ny + nh);
}
static bool hitDailyNext(int16_t x, int16_t y) {
  if (s_screen != SCREEN_DAILY) return false;
  int nx, ny, nw, nh;
  getDailyNavRect(nx, ny, nw, nh);
  int arrowTop = ny + DAILY_NAV_BACK_H;
  return (x >= nx + nw / 2 && x <= nx + nw && y >= arrowTop && y <= ny + nh);
}

// Shared geometry for the daily event list right column. Used by both
// renderDailyView() and the touch hit-test to avoid coordinate drift.
static void getDailyListRect(int& listX, int& listY, int& listW, int& listH) {
  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);
  int contentTop = cy + DAILY_STATUS_H + 8;
  listX = cx + DAILY_PAGE_MARGIN + DAILY_PAGE_SIZE + DAILY_LIST_GAP;
  listY = contentTop;
  listW = cx + cw - DAILY_PAGE_MARGIN - listX;
  listH = (EPD_HEIGHT - 4) - listY - 4;
}

// Hit test for event row taps in the daily list. Returns the global event
// index (into s_events[]) of the tapped row, or -1 if the tap is outside
// the list area or beyond the visible rows.
static int hitDailyEventRow(int16_t x, int16_t y) {
  if (s_screen != SCREEN_DAILY || s_selectedEventIdx >= 0) return -1;
  int listX, listY, listW, listH;
  getDailyListRect(listX, listY, listW, listH);
  if (x < listX || x > listX + listW || y < listY) return -1;
  int row = (y - listY) / DAILY_ROW_H;
  int maxRows = listH / DAILY_ROW_H;
  if (row < 0 || row >= s_dailyCount || row >= maxRows) return -1;
  return s_dailyIndices[row];
}

// Hit test for the "Back to list" button in daily detail mode.
static bool hitDailyDetailBack(int16_t x, int16_t y) {
  if (s_screen != SCREEN_DAILY || s_selectedEventIdx < 0) return false;
  int listX, listY, listW, listH;
  getDailyListRect(listX, listY, listW, listH);
  return (x >= listX && x <= listX + listW && y >= listY && y <= listY + 40);
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
    return;
  }

  if (dur > TAP_MAX_MS) {
    return;
  }

  if (s_screen == SCREEN_DAILY) {
    // Container nav (works in both list and detail mode)
    if (hitDailyBackToWeek(s_startX, s_startY)) {
      s_selectedEventIdx = -1;
      triggerScreenChange(SCREEN_WEEKLY, s_baseDayOffset);
      return;
    }
    if (hitDailyPrev(s_startX, s_startY)) {
      triggerNav(-1);
      return;
    }
    if (hitDailyNext(s_startX, s_startY)) {
      triggerNav(+1);
      return;
    }

    if (s_selectedEventIdx >= 0) {
      // Detail mode — "Back to list" button
      if (hitDailyDetailBack(s_startX, s_startY)) {
        s_selectedEventIdx = -1;
        s_pendingRender = true;
        s_refreshMode = REFRESH_PARTIAL_DAILY;
        s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
        return;
      }
    } else {
      // List mode — tap an event row to open detail
      int eventIdx = hitDailyEventRow(s_startX, s_startY);
      if (eventIdx >= 0) {
        s_selectedEventIdx = eventIdx;
        s_pendingRender = true;
        s_refreshMode = REFRESH_PARTIAL_DAILY;
        s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
        s_awaitingRelease = true;
        s_awaitingReleaseMs = millis();
        return;
      }
    }
  }

  // Weekly column tap → open daily view
  if (s_screen == SCREEN_WEEKLY) {
    int cx, cy, cw, ch;
    getContentRect(cx, cy, cw, ch);
    if (s_startY >= cy && s_startY <= EPD_HEIGHT - 4 &&
        s_startX >= cx && s_startX <= cx + cw) {
      // focus+context layout has variable column widths.
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
        return;
      } else if (col == 2) {
        // Right context column = forward arrow
        triggerNav(+1);
        return;
      } else {
        // col == 1, focus column = open daily view for the focus day
        s_selectedEventIdx = -1;
        triggerScreenChange(SCREEN_DAILY, s_baseDayOffset);
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
  if (s_screen == SCREEN_SETTINGS) {
    if (isTouched && !s_wasSettingsTouched) {
      ui_settings::TapResult result = ui_settings::handleTap(x, y);
      if (result == ui_settings::TAP_CLOSE) {
        s_screen = s_prevScreen;
        s_pendingRender = true;
        s_refreshMode = REFRESH_FULL;
        s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
        // The closing tap leaves a finger on the panel. Gate further input
        // until it lifts so it isn't classified as a spurious weekly/daily
        // gesture now that we've left the settings screen.
        s_awaitingRelease = true;
        s_awaitingReleaseMs = millis();
        s_phase = TOUCH_IDLE;
      } else if (result == ui_settings::TAP_FULL) {
        s_pendingRender = true;
        s_refreshMode = REFRESH_PARTIAL_SETTINGS;
      } else if (result == ui_settings::TAP_PARTIAL) {
        s_pendingRender = true;
        s_refreshMode = REFRESH_PARTIAL_SETTINGS;
      }
    }
    s_wasSettingsTouched = isTouched;
    return;
  }

  // Release gate: after a gesture, wait for the finger to be confirmed lifted
  // before accepting new touches. This prevents stale GT911 data (buffered
  // during the multi-second e-paper render) from triggering a duplicate action.
  // The 5-second timeout prevents permanent lockup if INT gets stuck.
  if (s_awaitingRelease) {
    if (!isTouched || millis() - s_awaitingReleaseMs > 5000) {
      s_awaitingRelease = false;
      s_phase = TOUCH_IDLE;
    }
    return;
  }

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
        s_awaitingRelease = true;
        s_awaitingReleaseMs = millis();
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
  int mode = (int)s_refreshMode;
  s_refreshMode = REFRESH_FULL;  // reset so unrelated renders default to full
  return mode;
}

void getSettingsDirtyRect(int& x, int& y, int& w, int& h) {
  ui_settings::getDirtyRect(x, y, w, h);
}

void getDailyDirtyRect(int& x, int& y, int& w, int& h) {
  int listX, listY, listW, listH;
  getDailyListRect(listX, listY, listW, listH);
  // Pad slightly beyond the list bounds for anti-aliased text edges.
  // 4-pixel alignment is handled by partialRefresh() itself.
  x = listX - 4;
  y = listY - 4;
  w = listW + 8;    // 4px each side
  h = listH + 4;    // 4px top only
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
// Weekly view (focus+context layout)
// ---------------------------------------------------------------------------
static void renderWeeklyView() {
  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  int winX, winY, winW, winH;
  getWindowRect(winX, winY, winW, winH);

  int cx, cy, cw, ch;
  getContentRect(cx, cy, cw, ch);
  const int weeklyCh = (EPD_HEIGHT - 4) - cy;   // footer is gone — columns fill to the bottom

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
  int timelineH = weeklyCh - headerH - 8;  // 8 = 4 px top gap + 4 px bottom padding

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

    // Footer is gone — every column now extends to the bottom of the screen.
    int effectiveCh = weeklyCh;

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

    // Separator between the date header and the column content.
    epd_draw_hline(x, cy + headerH, colW, EPD_BLACK, fb);

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

    // ----- All-day banner titles (collected here, drawn in both paths) -----
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

    // Context columns: simple chronological text list.
    if (!isFocus) {
      drawAllDayBanner(allDayTitles, allDayTitleCount, day, x, colW, timelineTop, fb);

      int listStartY = timelineTop;
      if (allDayTitleCount > 0) listStartY = timelineTop + 40;
      int listEndY = timelineTop + timelineH - 4;

      drawContextList(colIndices, colCount, x, colW, listStartY, listEndY, fb);
      continue;
    }

    // --- Focus column only below this point ---

    // Compute timeline range from the focus day's timed events.
    // Duration snaps up to a "nice" value from {3,6,9,12,18,24} hours
    // for consistent grid line spacing and visual rhythm.
    // Start stays fixed (earliest event - 1h), end extends to fill the nice duration.
    int focusRangeStart = 8 * 60;   // default: 8AM (all-day-only fallback)
    int focusRangeEnd = 20 * 60;    // default: 8PM (12h default)
    {
      int earliest = 24 * 60;
      int latest = 0;
      bool hasTimed = false;
      for (int e = 0; e < colCount; e++) {
        const CalendarEvent& ev = s_events[colIndices[e]];
        if (ev.allDay) continue;
        hasTimed = true;
        int s = ev.startHour * 60 + ev.startMin;
        int en = s + ev.durationMin;
        if (s < earliest) earliest = s;
        if (en > latest) latest = en;
      }
      if (hasTimed) {
        focusRangeStart = max(4 * 60, earliest - 60);     // no earlier than 4AM
        int paddedEnd = min(28 * 60, latest + 60);         // no later than 4AM next day
        int duration = paddedEnd - focusRangeStart;

        // Snap duration up to the next nice value
        const int niceDurations[] = {3*60, 6*60, 9*60, 12*60, 18*60, 24*60};
        int niceDuration = 24 * 60;
        for (int i = 0; i < 6; i++) {
          if (niceDurations[i] >= duration) {
            niceDuration = niceDurations[i];
            break;
          }
        }

        // Extend end to fit the nice duration (start stays fixed)
        focusRangeEnd = focusRangeStart + niceDuration;

        // Clamp to 4AM-4AM ceiling
        if (focusRangeEnd > 28 * 60) {
          focusRangeEnd = 28 * 60;
          focusRangeStart = focusRangeEnd - niceDuration;
          if (focusRangeStart < 4 * 60) {
            focusRangeStart = 4 * 60;
            focusRangeEnd = focusRangeStart + niceDuration;
          }
        }
      }
    }
    int focusRangeMinutes = focusRangeEnd - focusRangeStart;

    // Focus column timeline geometry: extends to the bottom of the screen.
    // When there's an all-day banner, content starts below it with a small gap.
    // Focus column layout: [optional banner] [top label] [event area] [bottom label]
    // Symmetric label areas at top and bottom ensure time labels are never
    // covered by event blocks.
    int focusContentTop = timelineTop;
    if (allDayTitleCount > 0) focusContentTop = timelineTop + 48;  // 36px banner + 12px gap
    else                       focusContentTop = timelineTop + 12;  // small top gap, no banner

    // Timeline boundaries: minimal top padding (the start label extends upward
    // into the banner gap), full bottom padding for the end label.
    int focusTimelineTop  = focusContentTop + 4;
    int focusTimelineBot  = (EPD_HEIGHT - 4) - 20;
    int focusTimelineH    = focusTimelineBot - focusTimelineTop;

    // Scale minimum block height to timeline density. In dense timelines
    // (short range, many pixels per hour), the default MIN_BLOCK_HEIGHT
    // causes events to overflow past their time slot, eating into gaps.
    // Scale it down so blocks don't exceed their proportional slot.
    int pxPerHour = focusTimelineH / max(1, focusRangeMinutes / 60);
    int effectiveMinH = max(40, min(MIN_BLOCK_HEIGHT, pxPerHour));

    // Timeline boundary lines with integrated time labels.
    // The start and end lines pass through the label text with a gap:
    //   ────── 8:00 AM ──────
    {
      int centerX = x + colW / 2;

      // Start boundary (top of timeline)
      char startLabel[16];
      formatTime(focusRangeStart / 60, focusRangeStart % 60, startLabel, sizeof(startLabel));
      int startTextW = measureTextWidth((GFXfont*)&MeltSwashes14, startLabel);
      int startHalfGap = startTextW / 2 + 8;
      epd_draw_hline(x + 8, focusTimelineTop, (centerX - startHalfGap) - (x + 8), EPD_LTGRAY, fb);
      epd_draw_hline(centerX + startHalfGap, focusTimelineTop,
                     (x + colW - 8) - (centerX + startHalfGap), EPD_LTGRAY, fb);
      drawCenteredTextColored(startLabel, centerX, focusTimelineTop + 11,
                              (GFXfont*)&MeltSwashes14, fb, C_LTGRAY, C_WHITE);

      // End boundary (bottom of timeline)
      char endLabel[16];
      formatTime(focusRangeEnd / 60, focusRangeEnd % 60, endLabel, sizeof(endLabel));
      int endTextW = measureTextWidth((GFXfont*)&MeltSwashes14, endLabel);
      int endHalfGap = endTextW / 2 + 8;
      epd_draw_hline(x + 8, focusTimelineBot, (centerX - endHalfGap) - (x + 8), EPD_LTGRAY, fb);
      epd_draw_hline(centerX + endHalfGap, focusTimelineBot,
                     (x + colW - 8) - (centerX + endHalfGap), EPD_LTGRAY, fb);
      drawCenteredTextColored(endLabel, centerX, focusTimelineBot + 11,
                              (GFXfont*)&MeltSwashes14, fb, C_LTGRAY, C_WHITE);
    }

    drawAllDayBanner(allDayTitles, allDayTitleCount, day, x, colW, timelineTop, fb);

    // Compute lane assignments for this day's events (focus column only).
    LaneAssignment laneAssignments[32];
    computeLaneAssignments(s_events, colIndices, colCount, laneAssignments);

    // Draw gap indicators between events with a >60 minute time gap.
    // Each gap gets a faint gray fill.
    // Drawn before event blocks so events render on top.
    {
      int prevEndMin = -1;
      for (int e = 0; e < colCount; e++) {
        const CalendarEvent& ev = s_events[colIndices[e]];
        if (ev.allDay) continue;

        int startMin = ev.startHour * 60 + ev.startMin;
        if (prevEndMin > 0 && (startMin - prevEndMin) >= 60) {
          int prevEndY = focusTimelineTop
                         + ((prevEndMin - focusRangeStart) * focusTimelineH) / focusRangeMinutes;
          int thisStartY = focusTimelineTop
                           + ((startMin - focusRangeStart) * focusTimelineH) / focusRangeMinutes;
          int gapY = prevEndY + EVENT_GAP;
          int gapH = thisStartY - prevEndY - EVENT_GAP * 2;
          if (gapH > 0) {
            // Faint gray fill (shade 14, just barely darker than white)
            epd_fill_rect(x + 6, gapY, colW - 12, gapH, 14 << 4, fb);
          }
        }

        int endMin = startMin + ev.durationMin;
        if (endMin > prevEndMin) prevEndMin = endMin;
      }
    }

    // Grid lines at 1/3 and 2/3 of the timeline — drawn after gap fills
    // so the shade-14 fill doesn't overwrite them. Events render on top.
    {
      int grid1Y = focusTimelineTop + focusTimelineH / 3;
      int grid2Y = focusTimelineTop + (focusTimelineH * 2) / 3;
      epd_draw_hline(x + 8, grid1Y, colW - 16, EPD_LTGRAY, fb);
      epd_draw_hline(x + 8, grid2Y, colW - 16, EPD_LTGRAY, fb);
    }

    for (int e = 0; e < colCount; e++) {
      const CalendarEvent& ev = s_events[colIndices[e]];

      if (ev.allDay) continue;  // all-day events handled in the banner above

      int startMin = ev.startHour * 60 + ev.startMin;
      int endMin   = startMin + ev.durationMin;
      int visStart = max(startMin, focusRangeStart);
      int visEnd   = min(endMin, focusRangeEnd);
      int visDur   = visEnd - visStart;
      if (visDur <= 0) continue;

      int blockY = focusTimelineTop + ((visStart - focusRangeStart) * focusTimelineH) / focusRangeMinutes;
      int blockH = computeBlockHeight(visDur, focusTimelineH, focusRangeMinutes, effectiveMinH);

      // Enforce minimum visual gap between non-overlapping events.
      // If the next event starts after this one ends (time gap exists),
      // clamp the block height so at least MIN_GAP_PX of white space
      // remains between them.
      {
        const int MIN_GAP_PX = 16;
        for (int e2 = e + 1; e2 < colCount; e2++) {
          const CalendarEvent& nextEv = s_events[colIndices[e2]];
          if (nextEv.allDay) continue;
          int nextStartMin = nextEv.startHour * 60 + nextEv.startMin;
          // Only enforce gap if there's actually a time gap (not overlapping)
          if (nextStartMin > endMin) {
            int nextVisStart = max(nextStartMin, focusRangeStart);
            int nextY = focusTimelineTop + ((nextVisStart - focusRangeStart) * focusTimelineH) / focusRangeMinutes;
            int maxH = nextY - blockY - MIN_GAP_PX;
            if (blockH > maxH) blockH = maxH;
          }
          break;  // only check the immediately next event
        }
      }

      int timelineBottom = focusTimelineBot;
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

      // Narrow the block to its lane width if multiple lanes (lane splitting).
      {
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

      // ----- 2-line rendering for events > 60 min -----
      char timeStr[24];
      formatTimeRange(ev.startHour, ev.startMin, ev.durationMin,
                      timeStr, sizeof(timeStr));

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

    }

  }
}

// ---------------------------------------------------------------------------
// Daily view (list + event detail)
// ---------------------------------------------------------------------------
static int eventSortKey(const CalendarEvent& ev) {
  if (ev.allDay) return -1;
  return ev.startHour * 60 + ev.startMin;
}

static void drawDailyNav(uint8_t* fb) {
  int nx, ny, nw, nh;
  getDailyNavRect(nx, ny, nw, nh);

  // Outer border
  epd_draw_rect(nx, ny, nw, nh, EPD_BLACK, fb);

  // "Back to Week" (top row)
  epd_fill_rect(nx, ny, nw, DAILY_NAV_BACK_H, EPD_WHITE, fb);
  epd_draw_rect(nx, ny, nw, DAILY_NAV_BACK_H, EPD_BLACK, fb);
  {
    const char* s = "Return";
    int32_t tw = measureTextWidth((GFXfont*)&MeltSwashes18, s);
    int32_t tx = nx + (nw - tw) / 2;
    int32_t ty = ny + 42;
    writeln((GFXfont*)&MeltSwashes18, s, &tx, &ty, fb);
  }

  // Horizontal divider between the two rows
  epd_draw_hline(nx, ny + DAILY_NAV_BACK_H, nw, EPD_BLACK, fb);
  // Vertical divider between the two arrow halves
  epd_draw_vline(nx + nw / 2, ny + DAILY_NAV_BACK_H, DAILY_NAV_ARROW_H, EPD_BLACK, fb);

  // Prev / Next arrows (reuse drawArrowButton). Each half is nw/2 wide.
  int arrowSize = 44;
  int arrowRowY = ny + DAILY_NAV_BACK_H;
  // Left half (prev)
  {
    int hx = nx;
    int cxArrow = hx + nw / 4;
    int cyArrow = arrowRowY + (DAILY_NAV_ARROW_H - arrowSize) / 2;
    drawArrowButton(cxArrow - arrowSize / 2, cyArrow, arrowSize, arrowSize, true, fb);
  }
  // Right half (next)
  {
    int hx = nx + nw / 2;
    int cxArrow = hx + nw / 4;
    int cyArrow = arrowRowY + (DAILY_NAV_ARROW_H - arrowSize) / 2;
    drawArrowButton(cxArrow - arrowSize / 2, cyArrow, arrowSize, arrowSize, false, fb);
  }
}

static void renderDailyView() {
  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

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

  // Sort by start time (all-day first)
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - 1 - i; j++) {
      if (eventSortKey(s_events[indices[j]]) > eventSortKey(s_events[indices[j+1]])) {
        int tmp = indices[j];
        indices[j] = indices[j+1];
        indices[j+1] = tmp;
      }
    }
  }

  // Cache for touch hit-testing
  s_dailyCount = count;
  for (int i = 0; i < count; i++) s_dailyIndices[i] = indices[i];

  int timedCount = 0, allDayCount = 0;
  for (int e = 0; e < count; e++) {
    if (s_events[indices[e]].allDay) allDayCount++;
    else timedCount++;
  }

  // -- Status strip -------------------------------------------------
  int statusY = cy;
  epd_fill_rect(cx, statusY, cw, DAILY_STATUS_H, EPD_LTGRAY, fb);
  epd_draw_rect(cx, statusY, cw, DAILY_STATUS_H, EPD_BLACK, fb);
  char statusBuf[48];
  snprintf(statusBuf, sizeof(statusBuf), "Events: %d | All-day: %d", timedCount, allDayCount);
  {
    FontProperties sp;
    sp.fg_color = C_BLACK; sp.bg_color = C_LTGRAY; sp.flags = 0; sp.fallback_glyph = 0;
    int32_t sx = cx + 12, sy = statusY + 22;
    write_mode((GFXfont*)&MeltSwashes18, statusBuf, &sx, &sy, fb, BLACK_ON_WHITE, &sp);
  }

  int contentTop = statusY + DAILY_STATUS_H + 8;

  // -- Left column: calendar + nav container ----------------------
  int pageX = cx + DAILY_PAGE_MARGIN;
  int pageY = contentTop;
  drawTearOffCalendar(pageX, pageY, DAILY_PAGE_SIZE, day, fb);

  // Nav container (Back to Week + prev/next day) below the calendar.
  drawDailyNav(fb);

  // -- Right column: event list or event detail --------------------
  int listX, listY, listW, listH;
  getDailyListRect(listX, listY, listW, listH);

  if (s_selectedEventIdx >= 0 && s_selectedEventIdx < s_eventCount) {
    // ===== Detail mode =====
    const CalendarEvent& ev = s_events[s_selectedEventIdx];

    // "← Back to list" button
    epd_fill_rect(listX, listY, listW, 40, EPD_DKGRAY, fb);
    epd_draw_rect(listX, listY, listW, 40, EPD_BLACK, fb);
    {
      const char* backStr = "← Back to list";
      FontProperties bp;
      bp.fg_color = C_WHITE; bp.bg_color = C_DKGRAY; bp.flags = 0; bp.fallback_glyph = 0;
      int32_t bx = listX + 16, by = listY + 28;
      write_mode((GFXfont*)&MeltSwashes18, backStr, &bx, &by, fb, BLACK_ON_WHITE, &bp);
    }

    // Event details below the back button
    int detailY = listY + 60;

    // Title (Genty24)
    char titleFit[64];
    truncateToFitWidth((GFXfont*)&Genty24, ev.title, listW - 32, titleFit, sizeof(titleFit));
    {
      int32_t tx = listX + 16, ty = detailY + 34;
      writeln((GFXfont*)&Genty24, titleFit, &tx, &ty, fb);
    }

    // Time range or "All day" (MeltSwashes18)
    detailY += 60;
    if (ev.allDay) {
      drawTextColored((GFXfont*)&MeltSwashes18, "All day",
                      listX + 16, detailY, fb, C_DKGRAY, C_WHITE);
    } else {
      char timeStr[32];
      formatTimeRange(ev.startHour, ev.startMin, ev.durationMin, timeStr, sizeof(timeStr));
      drawTextColored((GFXfont*)&MeltSwashes18, timeStr,
                      listX + 16, detailY, fb, C_DKGRAY, C_WHITE);
    }

    // Location (MeltSwashes16)
    detailY += 32;
    if (ev.location && ev.location[0]) {
      char locBuf[80];
      snprintf(locBuf, sizeof(locBuf), "Location: %s", ev.location);
      char locFit[80];
      truncateToFitWidth((GFXfont*)&MeltSwashes16, locBuf, listW - 32, locFit, sizeof(locFit));
      drawTextColored((GFXfont*)&MeltSwashes16, locFit,
                      listX + 16, detailY, fb, C_BLACK, C_WHITE);
    }

    // Description (MeltSwashes16, word-wrapped, if present)
    detailY += 28;
    if (ev.description && ev.description[0]) {
      drawTextColored((GFXfont*)&MeltSwashes16, "Details:",
                      listX + 16, detailY, fb, C_DKGRAY, C_WHITE);
      detailY += 24;
      WrappedText wrapped = wrapText((GFXfont*)&MeltSwashes16, ev.description,
                                      listW - 32, 3);
      for (int li = 0; li < wrapped.count; li++) {
        drawTextColored((GFXfont*)&MeltSwashes16, wrapped.lines[li],
                        listX + 16, detailY, fb, C_BLACK, C_WHITE);
        detailY += 22;
      }
    }

    // Calendar source (MeltSwashes14)
    detailY += 24;
    if (ev.calendar && ev.calendar[0]) {
      char calBuf[48];
      snprintf(calBuf, sizeof(calBuf), "Calendar: %s", ev.calendar);
      drawTextColored((GFXfont*)&MeltSwashes14, calBuf,
                      listX + 16, detailY, fb, C_LTGRAY, C_WHITE);
    }

  } else {
    // ===== List mode: compact 2-line rows =====
    int rowY = listY;
    int displayed = 0;
    int maxRows = listH / DAILY_ROW_H;

    for (int i = 0; i < count; i++) {
      if (displayed >= maxRows) {
        int remaining = count - displayed;
        char moreBuf[32];
        snprintf(moreBuf, sizeof(moreBuf), "+%d more...", remaining);
        int32_t mx = listX + 12, my = rowY + 20;
        writeln((GFXfont*)&MeltSwashes18, moreBuf, &mx, &my, fb);
        break;
      }

      const CalendarEvent& ev = s_events[indices[i]];

      // Alternating row background
      uint8_t rowBg = (displayed % 2 == 1) ? C_LTGRAY : C_WHITE;
      if (rowBg == C_LTGRAY) {
        epd_fill_rect(listX, rowY, listW, DAILY_ROW_H, EPD_LTGRAY, fb);
      }
      epd_draw_rect(listX, rowY, listW, DAILY_ROW_H, EPD_LTGRAY, fb);

      // Line 1: time range or "All day"
      if (ev.allDay) {
        drawTextColored((GFXfont*)&MeltSwashes18, "All day",
                        listX + 12, rowY + 24, fb, C_DKGRAY, rowBg);
      } else {
        char timeStr[32];
        formatTimeRange(ev.startHour, ev.startMin, ev.durationMin, timeStr, sizeof(timeStr));
        drawTextColored((GFXfont*)&MeltSwashes18, timeStr,
                        listX + 12, rowY + 24, fb, C_DKGRAY, rowBg);
      }

      // Line 2: title (truncated to fit)
      char titleFit[64];
      truncateToFitWidth((GFXfont*)&MeltSwashes16, ev.title, listW - 24,
                         titleFit, sizeof(titleFit));
      drawTextColored((GFXfont*)&MeltSwashes16, titleFit,
                      listX + 12, rowY + 48, fb, C_BLACK, rowBg);

      rowY += DAILY_ROW_H;
      displayed++;
    }
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void render() {
  if (s_screen != SCREEN_SETTINGS && (s_events == nullptr || s_eventCount == 0)) {
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
    case SCREEN_WEEKLY:   renderWeeklyView();   break;
    case SCREEN_DAILY:    renderDailyView();    break;
    case SCREEN_SETTINGS: ui_settings::render(); break;
    default:              renderWeeklyView();   break;
  }
}

void setEvents(const CalendarEvent* events, int count) {
  s_events = events;
  s_eventCount = count;
  s_pendingRender = true;
}

void toggleSettings() {
  // The button is independent of touch; reset the gesture machine so a finger
  // that was down when the button was pressed doesn't register as a spurious
  // tap when the modal later closes.
  s_phase = TOUCH_IDLE;
  s_awaitingRelease = false;
  s_startX = s_lastX = 0;
  s_startY = s_lastY = 0;

  if (s_screen == SCREEN_SETTINGS) {
    // Close — restore the previous view with a full refresh.
    s_screen = s_prevScreen;
    s_pendingRender = true;
    s_refreshMode = REFRESH_FULL;
  } else {
    // Open — modal over the current view, partial refresh of the modal rect.
    s_prevScreen = s_screen;
    s_screen = SCREEN_SETTINGS;
    ui_settings::markFullRedraw();
    s_pendingRender = true;
    s_refreshMode = REFRESH_PARTIAL_SETTINGS;
  }
  s_cooldownUntilMs = millis() + GESTURE_COOLDOWN_MS;
}

void resetToDefaultView() {
  s_screen = SCREEN_WEEKLY;
  s_baseDayOffset = 0;
  s_selectedEventIdx = -1;
  s_pendingRender = true;
  s_refreshMode = REFRESH_FULL;
}

} // namespace ui
