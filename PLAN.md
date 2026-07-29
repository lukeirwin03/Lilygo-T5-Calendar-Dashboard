# Implementation Plan: Power Optimization + SD Card + Settings + History

> **STATUS: COMPLETED** — All phases in this plan are implemented.
> The active UI redesign plan is in [`UI_REDESIGN.md`](UI_REDESIGN.md).
> This file is kept for historical reference.

### What was implemented from this plan:
- **Phase 1 (Power)**: Deep sleep with timer + button wake. Light sleep scaffolded but not wired into the main loop.
- **Phase 2 (SD Card)**: Full SD card support — event caching, settings file, log files with auto-cleanup.
- **Phase 3 (Settings)**: Runtime settings loaded from SD card. 5 of 7 settings actually applied (network settings displayed but require reboot). Settings UI debug-gated behind long-press.
- **Phase 4 (Month View + History)**: `sd_storage::loadEventsForDate()` is implemented but not yet used in rendering. Month view not built.

The UI redesign ([`UI_REDESIGN.md`](UI_REDESIGN.md)) supersedes the rendering portions of this plan.

> **Battery:** 3.7V 5200mAh 19.24Wh
> **Target:** Maximize time between charges. Passive device first, interactive second.

---

## Phase 1: Power Optimization (immediate battery benefit)

### 1A. Light sleep between touch polls
**Problem:** `delay(10)` in the main loop keeps the ESP32-S3 CPU fully clocked at
240 MHz even when nobody is touching the screen. At ~80 mA continuous, a 5-minute
idle interactive session burns ~7 mAh.

**Fix:** Replace `delay(10)` with `esp_light_sleep_start()` using a 10ms wakeup
timer AND the GT911 INT pin (GPIO47) as an edge wake source. The CPU drops to
~0.8 mA between polls and wakes instantly when a finger touches the panel.

- Files: `main_dashboard.cpp`, `main.cpp`, `touch_input.cpp/h`, `power_mgr.cpp/h`
- Add a `touch_input::enableLightSleepWake()` function that configures GPIO47
  as a wake source via `esp_sleep_enable_ext0_wakeup` or `esp_sleep_enable_gpio_wakeup`
- Add a `power_mgr::lightSleep(ms)` function that enters light sleep for a brief
  duration, waking on timer OR touch INT
- Replace `delay(10)` with `power_mgr::lightSleep(10)` in both loop functions
- Verify WiFi is fully off before entering light sleep (it is — we disconnect
  after payload fetch)

### 1B. CPU frequency reduction during interactive loop
**Problem:** CPU runs at 240 MHz constantly, but the touch-polling loop needs
almost no compute power.

**Fix:** Drop to 80 MHz during interactive loop. Bump back to 240 MHz only for
the render pass (drawing to framebuffer + e-paper refresh).

- In the loop: `setCpuFrequencyMhz(80)` before the touch poll section
- Before `doRender()`: `setCpuFrequencyMhz(240)`
- After `doRender()`: `setCpuFrequencyMhz(80)`
- ESP32-S3 at 80 MHz draws ~30 mA vs ~80 mA at 240 MHz

### 1C. Reduce inactivity timeout
- Change `INACTIVITY_TIMEOUT_MS` default from 5 min to 45 sec
- This will be configurable in Phase 3 settings
- With light sleep, even a longer timeout is cheap, but 45 sec is good UX for
  a passive device — glance at it, it sleeps

### 1D. Reduce scheduled wake frequency (optional)
- Current: every 2 hours = 12 wakes/day
- Could change to every 4 hours = 6 wakes/day
- Saves ~1 mAh/day (minor — deep sleep baseline dominates)
- Make this configurable in settings

**Estimated impact:** Interactive power drops from ~5-20 mAh/day to ~0.5 mAh/day.
With a 5200 mAh battery, projected life goes from 55-167 days to 70-193 days.

---

## Phase 2: SD Card Module

### Hardware
- SD card pins (from config.h): MISO=16, MOSI=15, SCK=11, CS=42
- SPI mode, FAT32 formatted card
- 32 GB card holds ~60,000 years of event data (500 KB/year)

### 2A. SD card driver (`include/sd_storage.h`, `src/sd_storage.cpp`)

```
namespace sd_storage {
  bool begin();                    // mount SD card over SPI
  bool isMounted();

  // Write per-day event files. Called on each MQTT payload received.
  // Extracts events grouped by date, writes one JSON file per date.
  bool saveEvents(const CalendarEvent* events, int count);

  // Load events for a specific date from SD card.
  // Returns event count, fills the out array. 0 = no file or empty.
  int loadEventsForDate(const char* dateStr, CalendarEvent* out, int maxOut);

  // Config file read/write (Phase 3)
  bool saveConfig(const char* json, size_t len);
  bool loadConfig(char* buf, size_t bufLen);
}
```

### 2B. File layout on SD card

```
/cal/
  2026-06-29.json     ← array of events for that date
  2026-06-30.json
  ...
/config/
  settings.json       ← user-configurable settings
```

Each event file format:
```json
[
  {
    "title": "Chiro",
    "location": "West Omaha Chiro",
    "description": "Routine adjustment",
    "calendar": "main",
    "type": "event",
    "start": "15:30",
    "end": "16:00",
    "all_day": false
  }
]
```

### 2C. Integration into main_dashboard.cpp

On every MQTT payload received (`calendarDash.dirty` in the loop):
1. `sd_storage::saveEvents(calendarDash.events(), calendarDash.eventCount())`
2. This iterates all events, groups by date, writes/updates per-day files
3. Only runs on scheduled wakes (WiFi is off during interactive, no new data)

### 2D. Power considerations
- SD card SPI bus is initialized once at boot
- Card is put to sleep between accesses (CS high + clock stopped)
- Writes happen only during scheduled wakes (every 2-4 hours)
- Each write session: 7 small files (~1-2 KB each), <100 ms total
- SD card idle current: ~200 µA (negligible vs deep sleep baseline)

---

## Phase 3: Settings UI

### 3A. Settings struct (`include/settings.h`, `src/settings.cpp`)

A runtime-modifiable settings struct that overrides config.h defaults:

```
struct Settings {
  // Network
  char wifi_ssid[33];
  char wifi_password[65];
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_topic[64];

  // Display
  uint8_t day_start_hour;      // default 7
  uint8_t day_end_hour;        // default 22

  // Power
  uint32_t refresh_interval_s;   // default 7200 (2 hours)
  uint32_t inactivity_timeout_s; // default 45
  uint32_t history_retention_d;  // default 365

  // Load from SD card at boot, fall back to defaults
  void load();
  void save() const;
  void applyDefaults();
};
```

At boot: `settings.load()` reads `/config/settings.json` from SD card.
If missing or corrupt, falls back to config.h compile-time defaults.
`settings.save()` writes back to SD card when user changes something.

### 3B. Settings screen (`src/ui_settings.cpp`)

A new screen type `SCREEN_SETTINGS` in the UI:

- Scrollable list of setting rows
- Each row: label (left) + current value + `−` / `+` buttons (right)
- For numeric/enum values: tap `−`/`+` to cycle
- For text values (SSID, host, topic): tap the value to open a character grid
  - Character grid shows letters/numbers/symbols in a tap-target layout
  - Like an old phone keyboard adapted for e-paper touch
  - Slow but functional — only needed when changing networks
- "Save & Exit" button at bottom writes settings.json to SD card
- Settings take effect on next wake (or immediately for display settings)

### 3C. Settings access via gear icon

- Small gear icon drawn on the bottom header bar (next to battery icon)
- Tap gear → enters settings screen
- Settings screen has "← Back" to return to weekly/daily view
- Touch state machine in ui.cpp gets a new `SCREEN_SETTINGS` case

### 3D. Settings that take effect immediately vs next wake

| Setting | When it takes effect |
|---------|---------------------|
| day_start_hour / day_end_hour | Next render |
| inactivity_timeout_s | Immediately (loop checks the value) |
| wifi_ssid / mqtt_host / etc. | Next scheduled wake (device reconnects) |
| refresh_interval_s | Next sleep cycle |
| history_retention_d | Next SD card cleanup pass |

---

## Phase 4: Month View + History Browsing

### 4A. Calendar icon on header

- Small calendar/grid icon on the bottom header bar (next to gear icon)
- Tap → opens month view
- Present on weekly and daily screens

### 4B. Month view (`src/ui_month.cpp`)

```
┌──────────────────────────────────────────┐
│  ‹    June 2026              [✕ Close]    │
├──────────────────────────────────────────┤
│        S    M    T    W    T    F    S   │
│   ┌──────────────────────────────────┐   │
│   │                           1    2  │ ◀─ tap day = daily view
│   │  3    4    5    6    7    8    9  │   │
│   │ 10   11   12   13   14   15   16  │   │
│   │ 17   18   19   20   21   22   23  │   │
│   │ 24   25   26   27   28   29   30  │   │
│   └──────────────────────────────────┘   │
│   [Wk1] [Wk2] [Wk3] [Wk4] [Wk5]          │ ◀─ tap week = weekly view
│                                          │
│  ‹ June    July ›                        │
└──────────────────────────────────────────┘
```

- Month/year displayed at top with `‹`/`›` arrows to navigate months
- Days with archived events shown in bold or with a dot indicator
- Tap a day number → opens daily view for that date
- Each week row has a `[Wk]` button on the left → opens weekly view
- `[✕ Close]` returns to previous view
- Title shows date range or "(archived)" when loading from SD card

### 4C. Loading historical events

When the user navigates to a date via month view or scrolls backward:
1. Check if the date is within the current MQTT payload's event array
2. If yes → use those events (fast, in-memory)
3. If no → call `sd_storage::loadEventsForDate()` to load from SD card
4. Feed loaded events to `ui::setEvents()` for rendering
5. Title shows "(archived)" indicator so user knows it's historical data

### 4D. Auto-cleanup (history retention)

On cold boot, check `settings.history_retention_d`:
- Scan `/cal/` directory for files older than the retention period
- Delete expired files
- Runs once per cold boot (not every wake — minimize SD card writes)

---

## Implementation Order

1. **Phase 1A-1C** — Power optimization (light sleep, CPU freq, timeout)
   - Immediate battery benefit
   - No new files needed, just modifies existing loop code
   - Test: measure current during interactive idle

2. **Phase 2A-2C** — SD card module
   - Foundation for settings and history
   - New files: sd_storage.h/cpp
   - Test: verify events written to SD card after MQTT payload

3. **Phase 3A-3D** — Settings system
   - New files: settings.h/cpp, ui_settings.cpp
   - Modify: ui.cpp (add gear icon + SCREEN_SETTINGS)
   - Test: change a setting on-device, verify it persists across sleep

4. **Phase 4A-4D** — Month view + history
   - New files: ui_month.cpp
   - Modify: ui.cpp (add calendar icon + SCREEN_MONTH)
   - Test: browse to a date 3 months ago, verify events load from SD card

---

## File inventory after all phases

```
include/
  config.h              ← compile-time defaults (fallback)
  settings.h            ← runtime settings struct + load/save     [NEW]
  sd_storage.h          ← SD card read/write API                   [NEW]
  ui.h                  ← main UI namespace (existing)
  ui_settings.h        ← settings screen API                       [NEW]
  ui_month.h           ← month view API                            [NEW]
  ... (existing headers unchanged)

src/
  main_dashboard.cpp   ← modified: light sleep, SD init, settings load
  main.cpp             ← modified: light sleep
  ui.cpp               ← modified: gear/calendar icons, new screens
  ui_settings.cpp      ← settings screen rendering + touch         [NEW]
  ui_month.cpp         ← month view rendering + touch              [NEW]
  sd_storage.cpp       ← SD card implementation                    [NEW]
  settings.cpp         ← settings load/save implementation         [NEW]
  ... (existing source files unchanged)
```
