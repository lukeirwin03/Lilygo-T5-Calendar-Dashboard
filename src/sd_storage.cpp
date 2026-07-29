#include "sd_storage.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <time.h>

namespace sd_storage {

static SPIClass sdSPI(HSPI);
static bool mounted = false;

// ---------------------------------------------------------------------------
// Mount + directory setup + log cleanup
// ---------------------------------------------------------------------------
bool begin() {
  sdSPI.begin(config::SDCARD_SCK, config::SDCARD_MISO, config::SDCARD_MOSI, config::SDCARD_CS);

  if (!SD.begin(config::SDCARD_CS, sdSPI, 4000000)) {
    Serial.println("[sd] Card mount failed");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[sd] No SD card attached");
    return false;
  }

  const char* typeStr = "UNKNOWN";
  if (cardType == CARD_MMC)  typeStr = "MMC";
  if (cardType == CARD_SD)   typeStr = "SD";
  if (cardType == CARD_SDHC) typeStr = "SDHC";

  Serial.printf("[sd] %s card, %.0f MB\n", typeStr,
                (double)SD.cardSize() / (1024 * 1024));

  SD.mkdir("/cal");
  SD.mkdir("/config");
  SD.mkdir("/logs");

  mounted = true;

  log("SD", "SD card mounted (%s, %.0f MB)", typeStr,
      (double)SD.cardSize() / (1024 * 1024));

  return true;
}

bool isMounted() { return mounted; }

// ---------------------------------------------------------------------------
// Logging — append to /logs/YYYY-MM-DD.log, also print to Serial
// ---------------------------------------------------------------------------
void log(const char* tag, const char* fmt, ...) {
  char msg[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  // Build timestamp
  char ts[32];
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
  } else {
    snprintf(ts, sizeof(ts), "epoch+%lu", millis() / 1000);
  }

  // Serial output
  Serial.printf("[%s] [%s] %s\n", ts, tag, msg);

  // SD card append
  if (mounted) {
    char datePart[16];
    if (now > 1700000000) {
      struct tm lt;
      localtime_r(&now, &lt);
      snprintf(datePart, sizeof(datePart), "%04d-%02d-%02d",
               lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    } else {
      strcpy(datePart, "boot");
    }

    char path[40];
    snprintf(path, sizeof(path), "/logs/%s.log", datePart);

    File f = SD.open(path, FILE_APPEND);
    if (f) {
      f.printf("[%s] [%s] %s\n", ts, tag, msg);
      f.close();
    }
  }
}

void cleanOldLogs(int maxDays) {
  if (!mounted) return;

  // Guard against clock not being set (e.g., cold boot before NTP sync).
  // Without this, time(nullptr) returns a tiny value, the cutoff underflows,
  // and ALL log files would be deleted.
  time_t now = time(nullptr);
  if (now < 1700000000) {  // before ~Nov 2023
    Serial.println("[sd] Skipping log cleanup — clock not set");
    return;
  }

  File dir = SD.open("/logs");
  if (!dir) return;

  time_t cutoff = now - (time_t)maxDays * 86400;

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    String name = entry.name();
    entry.close();

    // Parse date from filename: YYYY-MM-DD.log
    if (name.length() < 14) continue;
    int y, m, d;
    if (sscanf(name.c_str(), "%d-%d-%d", &y, &m, &d) != 3) continue;

    struct tm ft = {0};
    ft.tm_year = y - 1900;
    ft.tm_mon = m - 1;
    ft.tm_mday = d;
    time_t fileTime = mktime(&ft);

    if (fileTime < cutoff) {
      char path[40];
      snprintf(path, sizeof(path), "/logs/%s", name.c_str());
      SD.remove(path);
      Serial.printf("[sd] Deleted old log: %s\n", name.c_str());
    }
  }
  dir.close();
}

// ---------------------------------------------------------------------------
// Config file read/write
// ---------------------------------------------------------------------------
bool saveConfig(const char* json, size_t len) {
  if (!mounted) return false;

  File f = SD.open("/config/settings.json", FILE_WRITE);
  if (!f) return false;

  f.write((const uint8_t*)json, len);
  f.close();
  log("SD", "Settings saved");
  return true;
}

bool loadConfig(char* buf, size_t bufLen) {
  if (!mounted) return false;

  File f = SD.open("/config/settings.json", FILE_READ);
  if (!f) return false;

  size_t bytesRead = f.readBytes(buf, bufLen - 1);
  buf[bytesRead] = '\0';
  f.close();
  return bytesRead > 0;
}

} // namespace sd_storage
