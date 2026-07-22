#pragma once
#include "dashboards/calendar_dashboard.h"
#include <stddef.h>

namespace sd_storage {

// Mount the SD card over SPI and create required directories.
bool begin();
bool isMounted();

// Append a log message to /logs/YYYY-MM-DD.log (and Serial).
void log(const char* tag, const char* fmt, ...);

// Delete log files older than maxDays.
void cleanOldLogs(int maxDays);

// Persist events grouped by date to /cal/YYYY-MM-DD.json.
bool saveEvents(const CalendarEvent* events, int count);

// Load events for a specific date from /cal/YYYY-MM-DD.json.
// Returns the number of events loaded (0 if missing or empty).
int loadEventsForDate(const char* dateStr, CalendarEvent* out, int maxOut);

// Raw config file read/write used by settings.cpp.
bool saveConfig(const char* json, size_t len);
bool loadConfig(char* buf, size_t bufLen);

} // namespace sd_storage
