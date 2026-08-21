#pragma once
#include "dashboard.h"
#include <time.h>

// Enriched event with parsed times for layout.
struct CalendarEvent {
  char title[64];
  char location[48];
  char description[128];
  char calendar[24];   // source calendar name (e.g. "personal", "work")
  char type[16];       // "event" or "task"
  char date[11];       // YYYY-MM-DD (start date)
  int  startHour;      // 0-23
  int  startMin;       // 0-59
  int  durationMin;    // total duration in minutes
  bool allDay;
  uint8_t shade;       // 4-bit grayscale (0=black, 15=white)
};

class CalendarDashboard : public Dashboard {
public:
  CalendarDashboard();

  const char* topic() const override;
  const char* name() const override;
  void handlePayload(JsonDocument& doc) override;
  void render() override;
  void writeCacheFromCurrent() override;

  // Expose parsed events for external rendering (ui)
  const CalendarEvent* events() const { return events_; }
  int eventCount() const { return eventCount_; }
  time_t lastUpdated() const { return lastUpdated_; }

private:
  static constexpr int MAX_EVENTS = 256;
  CalendarEvent* events_ = nullptr;   // lazily allocated in clearData() (PSRAM w/ internal-RAM fallback)
  int eventCount_ = 0;
  time_t lastUpdated_ = 0;   // UTC epoch parsed from the payload's "updated" field (0 = unknown)

  void clearData();
  void addEvent(const char* title, const char* location, const char* description,
                const char* calendar, const char* type,
                const char* startIso, const char* endIso, bool allDay);
  void parseIsoDateTime(const char* iso, int& year, int& month, int& day,
                        int& hour, int& min);
  int  minutesBetween(int y1,int m1,int d1,int h1,int min1,
                      int y2,int m2,int d2,int h2,int min2);
  uint8_t shadeForCalendar(const char* name) const;

  // Phase 1 debug
  void dumpParsedEvents() const;
};
