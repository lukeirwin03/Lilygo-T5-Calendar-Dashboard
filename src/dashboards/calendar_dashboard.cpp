#include "dashboards/calendar_dashboard.h"
#include "config.h"
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
  eventCount_ = 0;
  memset(events_, 0, sizeof(events_));
}

void CalendarDashboard::addEvent(const char* title, const char* location,
                                 const char* description,
                                 const char* calendar, const char* type,
                                 const char* startIso, const char* endIso,
                                 bool allDay) {
  if (eventCount_ >= MAX_EVENTS) return;

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

void CalendarDashboard::handlePayload(JsonDocument& doc) {
  clearData();
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
      addEvent(title, location, description, calendar, type, start, end, allDay);
    }
  }
  hasData = true; dirty = true;
  Serial.printf("[calendar] Parsed %d raw events -> %d entries\n",
                (int)arr.size(), eventCount_);
#ifdef CORE_DEBUG_LEVEL
  #if CORE_DEBUG_LEVEL >= 4
    dumpParsedEvents();
  #endif
#endif
}

void CalendarDashboard::render() {}
