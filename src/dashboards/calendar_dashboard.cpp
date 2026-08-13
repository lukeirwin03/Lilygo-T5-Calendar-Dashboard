#include "dashboards/calendar_dashboard.h"
#include "config.h"
#include "settings.h"
#include "sd_storage.h"
#include <Arduino.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

CalendarDashboard::CalendarDashboard() {}

const char* CalendarDashboard::topic() const { return config::MQTT_TOPIC; }
const char* CalendarDashboard::name()  const { return "Calendar"; }

// ---------------------------------------------------------------------------
// ISO date/time parsing
// ---------------------------------------------------------------------------
void CalendarDashboard::parseIsoDateTime(const char* iso, int& year, int& month, int& day,
                                         int& hour, int& min) {
  year = month = day = 0;
  hour = min = 0;
  if (!iso || !iso[0]) return;
  if (sscanf(iso, "%d-%d-%d", &year, &month, &day) < 3) return;
  const char* t = strchr(iso, 'T');
  if (t) sscanf(t + 1, "%d:%d", &hour, &min);
}

// Parse a UTC ISO-8601 timestamp ("2026-08-15T19:05:00Z") to a UTC epoch.
// mktime() treats struct tm as LOCAL time, which would make the freshness age
// wrong by the local TZ offset — so we briefly switch the C lib TZ to UTC,
// convert, then restore. Single-threaded app: safe.
static time_t parseUtcIso(const char* iso) {
  if (!iso || !iso[0]) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 5) return 0;
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon  = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min  = mi;
  t.tm_sec  = s;
  t.tm_isdst = 0;
  const char* cur = getenv("TZ");
  char tzOld[32] = {0};
  if (cur) strlcpy(tzOld, cur, sizeof(tzOld));
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&t);
  if (cur) setenv("TZ", tzOld, 1); else unsetenv("TZ");
  tzset();
  return epoch;
}

int CalendarDashboard::minutesBetween(int y1,int m1,int d1,int h1,int min1,
                                      int y2,int m2,int d2,int h2,int min2) {
  struct tm t1 = {0};
  t1.tm_year = y1 - 1900; t1.tm_mon = m1 - 1; t1.tm_mday = d1;
  t1.tm_hour = h1; t1.tm_min = min1; t1.tm_isdst = -1;
  struct tm t2 = {0};
  t2.tm_year = y2 - 1900; t2.tm_mon = m2 - 1; t2.tm_mday = d2;
  t2.tm_hour = h2; t2.tm_min = min2; t2.tm_isdst = -1;
  time_t epoch1 = mktime(&t1);
  time_t epoch2 = mktime(&t2);
  if (epoch1 == (time_t)-1 || epoch2 == (time_t)-1) return 60;
  return (int)(difftime(epoch2, epoch1) / 60.0);
}

// ---------------------------------------------------------------------------
// Shade mapping
// ---------------------------------------------------------------------------
uint8_t CalendarDashboard::shadeForCalendar(const char* name) const {
  if (!name || !name[0]) return 7;
  unsigned long h = 5381;
  for (const char* p = name; *p; p++) h = ((h << 5) + h) + *p;
  // Spaced-apart shades for visible contrast on e-paper (no pure black for fills)
  const uint8_t shades[] = {3, 7, 10, 13};
  return shades[h % 4];
}

// ---------------------------------------------------------------------------
// Data management
// ---------------------------------------------------------------------------
void CalendarDashboard::clearData() {
  if (!events_) {
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    events_ = (CalendarEvent*)ps_malloc((size_t)MAX_EVENTS * sizeof(CalendarEvent));
#endif
    if (!events_) events_ = (CalendarEvent*)malloc((size_t)MAX_EVENTS * sizeof(CalendarEvent));
  }
  if (events_) memset(events_, 0, (size_t)MAX_EVENTS * sizeof(CalendarEvent));
  eventCount_ = 0;
}

void CalendarDashboard::addEvent(const char* title, const char* location,
                                 const char* description,
                                 const char* calendar, const char* type,
                                 const char* startIso, const char* endIso,
                                 bool allDay) {
  if (!events_ || eventCount_ >= MAX_EVENTS) {
    static bool warned = false;
    if (!warned) {
      Serial.printf("[calendar] WARN: MAX_EVENTS (%d) cap hit — '%s' and possibly later events dropped\n",
                    MAX_EVENTS, title ? title : "?");
      warned = true;
    }
    return;
  }

  int sy, sm, sd, sh, smin;
  int ey, em, ed, eh, emin;
  parseIsoDateTime(startIso, sy, sm, sd, sh, smin);
  parseIsoDateTime(endIso,   ey, em, ed, eh, emin);

  if (allDay) {
    struct tm cur = {0};
    cur.tm_year = sy - 1900; cur.tm_mon = sm - 1; cur.tm_mday = sd;
    cur.tm_hour = 0; cur.tm_min = 0; cur.tm_isdst = -1;
    struct tm endTm = {0};
    endTm.tm_year = ey - 1900; endTm.tm_mon = em - 1; endTm.tm_mday = ed;
    endTm.tm_isdst = -1;
    time_t startEpoch = mktime(&cur);
    time_t endEpoch   = mktime(&endTm);
    if (startEpoch == (time_t)-1 || endEpoch == (time_t)-1) return;

    while (difftime(endEpoch, startEpoch) > 0 && eventCount_ < MAX_EVENTS) {
      CalendarEvent& ev = events_[eventCount_];
      strlcpy(ev.title, title, sizeof(ev.title));
      strlcpy(ev.location, location ? location : "", sizeof(ev.location));
      strlcpy(ev.description, description ? description : "", sizeof(ev.description));
      strlcpy(ev.calendar, calendar ? calendar : "", sizeof(ev.calendar));
      strlcpy(ev.type, type ? type : "event", sizeof(ev.type));
      strftime(ev.date, sizeof(ev.date), "%Y-%m-%d", &cur);
      ev.startHour = 0; ev.startMin = 0;
      ev.durationMin = 24 * 60;
      ev.allDay = true;
      ev.shade = shadeForCalendar(ev.calendar);
      eventCount_++;
      cur.tm_mday++; mktime(&cur); startEpoch = mktime(&cur);
    }
    if (eventCount_ >= MAX_EVENTS && difftime(endEpoch, startEpoch) > 0) {
      Serial.printf("[calendar] WARN: all-day expansion of '%s' truncated at MAX_EVENTS cap\n",
                    title ? title : "?");
    }
  } else {
    CalendarEvent& ev = events_[eventCount_];
    strlcpy(ev.title, title, sizeof(ev.title));
    strlcpy(ev.location, location ? location : "", sizeof(ev.location));
    strlcpy(ev.description, description ? description : "", sizeof(ev.description));
    strlcpy(ev.calendar, calendar ? calendar : "", sizeof(ev.calendar));
    strlcpy(ev.type, type ? type : "event", sizeof(ev.type));
    snprintf(ev.date, sizeof(ev.date), "%04d-%02d-%02d", sy, sm, sd);
    ev.startHour = sh; ev.startMin = smin;
    ev.durationMin = minutesBetween(sy, sm, sd, sh, smin, ey, em, ed, eh, emin);
    if (ev.durationMin <= 0) ev.durationMin = 30;
    ev.allDay = false;
    ev.shade = shadeForCalendar(ev.calendar);
    eventCount_++;
  }
}

void CalendarDashboard::dumpParsedEvents() const {
  Serial.println("[calendar] === PARSED EVENTS ===");
  for (int i = 0; i < eventCount_; i++) {
    const CalendarEvent& ev = events_[i];
    Serial.printf("  [%d] '%s'  date=%s  %02d:%02d  dur=%dmin  allDay=%s  cal=%s  shade=%d\n",
                  i, ev.title, ev.date, ev.startHour, ev.startMin,
                  ev.durationMin, ev.allDay ? "yes" : "no", ev.calendar, ev.shade);
  }
  Serial.println("[calendar] ====================");
}

void CalendarDashboard::writeCacheFromCurrent() {
  if (!events_) return;
  // One cache file per unique event-date present in the current window.
  for (int i = 0; i < eventCount_; i++) {
    const char* d = events_[i].date;
    if (!d || !d[0]) continue;
    bool seen = false;
    for (int j = 0; j < i; j++) {
      if (strcmp(events_[j].date, d) == 0) { seen = true; break; }
    }
    if (seen) continue;
    sd_storage::saveDayCache(d, events_, eventCount_);
  }
}

void CalendarDashboard::handlePayload(JsonDocument& doc) {
  clearData();

  // Freshness: capture the payload's UTC "updated" timestamp so the UI can
  // show data age and warn when the broker has stopped publishing.
  lastUpdated_ = 0;
  const char* updatedStr = doc["updated"];
  if (updatedStr && updatedStr[0]) {
    lastUpdated_ = parseUtcIso(updatedStr);
  }

  time_t now = time(nullptr);
  bool clockValid = (now >= 1700000000);
  time_t windowStart = 0;
  time_t windowEnd = 0;
  int contextDays = settings::get().context_days;
  if (contextDays < 1) contextDays = 1;
  if (contextDays > config::MAX_CONTEXT_DAYS) contextDays = config::MAX_CONTEXT_DAYS;
  if (clockValid) {
    struct tm todayTm;
    localtime_r(&now, &todayTm);
    todayTm.tm_hour = 0;
    todayTm.tm_min = 0;
    todayTm.tm_sec = 0;
    time_t todayStart = mktime(&todayTm);
    // Bidirectional window: today, the N days before, and the N days after.
    // Half-open [start, end) so +contextDays is inclusive.
    windowStart = todayStart - (time_t)contextDays * 86400;
    windowEnd   = todayStart + (time_t)(contextDays + 1) * 86400;
  }

  int skippedCount = 0;
  JsonArray arr = doc["events"];
  if (!arr.isNull()) {
    for (JsonObject ev : arr) {
      const char* title       = ev["title"]       | "";
      const char* location    = ev["location"]    | "";
      const char* description = ev["description"] | "";
      const char* calendar    = ev["calendar"]    | "";
      const char* type        = ev["type"]        | "event";
      const char* start       = ev["start"]       | "";
      const char* end         = ev["end"]         | "";
      bool allDay             = ev["all_day"]     | false;
      if (!start[0]) continue;
      if (!end[0]) end = start;

      // Skip events outside the memory window (only when clock is valid).
      if (clockValid) {
        int y, m, d, h, min;
        parseIsoDateTime(start, y, m, d, h, min);
        struct tm evTm;
        memset(&evTm, 0, sizeof(evTm));
        evTm.tm_year = y - 1900;
        evTm.tm_mon = m - 1;
        evTm.tm_mday = d;
        evTm.tm_isdst = -1;
        time_t evDate = mktime(&evTm);
        if (evDate < windowStart || evDate >= windowEnd) {
          skippedCount++;
          continue;
        }
      }

      addEvent(title, location, description, calendar, type, start, end, allDay);
    }
  }

  // --- Augment with past days from the on-disk cache ---
  // The broker publishes forward-only; recent past days come from /cal/cache
  // so the bidirectional window has history to show. (Cold-start: the cache
  // is empty until events age in — expected by design.)
  if (clockValid && events_) {
    for (int offset = -contextDays; offset <= -1; offset++) {
      struct tm dayTm;
      localtime_r(&now, &dayTm);
      dayTm.tm_mday += offset;
      dayTm.tm_hour = 0;
      dayTm.tm_min = 0;
      dayTm.tm_sec = 0;
      mktime(&dayTm);
      char dateStr[11];
      strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &dayTm);
      int loaded = sd_storage::loadDayCache(dateStr,
                                            events_ + eventCount_,
                                            MAX_EVENTS - eventCount_);
      eventCount_ += loaded;
      if (eventCount_ >= MAX_EVENTS) break;  // respect the cap
    }
  }
  hasData = true; dirty = true;
  Serial.printf("[calendar] Parsed %d events (%d outside ±%d-day window) -> %d entries\n",
                (int)arr.size(), skippedCount, contextDays, eventCount_);
#ifdef CORE_DEBUG_LEVEL
  #if CORE_DEBUG_LEVEL >= 4
    dumpParsedEvents();
  #endif
#endif
}

void CalendarDashboard::render() {}
