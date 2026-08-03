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

// Raw config file read/write used by settings.cpp.
bool saveConfig(const char* json, size_t len);
bool loadConfig(char* buf, size_t bufLen);

// Raw calendar payload cache (/cal/current.json) — persists across power loss.
bool savePayload(const char* json, size_t len);
size_t loadPayload(char* buf, size_t bufLen);

// Append a payload snapshot to the daily history file
// (/cal/history/YYYY-MM-DD.jsonl) in JSONL format.
void appendHistory(const char* payload, size_t length);

// Delete history files older than maxDays.
void cleanOldHistory(int maxDays);

} // namespace sd_storage
