# T5 Calendar Dashboard — Current State

> **Last updated:** June 23, 2026
> **Purpose:** Accurate snapshot of the codebase after the cleanup audit.
> This file is the source of truth — `README.md` is stale and should not be
> trusted until it is rewritten.

---

## TL;DR

The project has **two build targets** selectable via PlatformIO environments:

| Target | Command | What it does |
| ------ | ------- | ------------ |
| **demo** (default) | `pio run -e demo` | UI concept work with hardcoded placeholder data. No WiFi, no MQTT, no sleep. |
| **dashboard** | `pio run -e dashboard` | Full calendar dashboard: WiFi, MQTT subscribe, SNTP, deep sleep, touch/button wake, RTC cache. |

The demo is the current active development target. The dashboard code is
preserved and builds independently. Both share the same display, touch,
battery, and power-manager modules.

---

## Hardware

| Part           | Detail                                                |
| -------------- | ----------------------------------------------------- |
| Board          | LilyGo T5 4.7" e-paper V2.3 (ESP32-S3)                |
| Panel          | 960 × 540 4-bit grayscale e-paper                     |
| Display driver | `LilyGo-EPD47` (`epd_driver.h`)                       |
| Touch          | GT911 capacitive touch (I²C, SDA=18, SCL=17)          |
| User button    | GPIO 21 (active-low, RTC-capable)                     |
| Battery ADC    | GPIO 14 (voltage divider, ratio 2.0×)                 |
| SD card pins   | MISO=16, MOSI=15, SCK=11, CS=42 (documented but unused) |

Board definition: [`boards/T5-ePaper-S3.json`](boards/T5-ePaper-S3.json)
(16 MB QIO flash, OPI PSRAM, ESP32-S3).

---

## Demo target (`pio run -e demo`)

`src/main.cpp` boots, initializes display/touch/battery, and enters an
interactive loop rendering one of two demo screens using **hardcoded
placeholder data** (7 fake events in `main.cpp`, rendered by `ui.cpp`).

### Demo weekly view (4 columns)

- Four day columns with a proportional mini-timeline from **7 AM to 10 PM**.
- Each column: short date header (`Mon 06/23`), hour grid lines every 3 hours,
  event blocks positioned by absolute clock time.
- All-day events: black bar at column bottom with white text.
- Event blocks: grayscale shades with time + truncated title. An experimental
  right-to-left glyph draw order (`drawTextRTL`) is applied to title text to
  work around overlapping bounding boxes in decorative fonts.
- **Bottom header bar** (striped pattern) with left/right arrows, battery icon,
  centered date-range title.

### Demo daily view

- **Tear-off calendar page** on the left (binding strip, rings, zigzag tear
  line, day name in Genty24, day number in Genty48).
- `< Week` back button below the tear-off page.
- Chronological event list on the right with alternating row backgrounds, time
  range, title, description.
- Same bottom header bar with arrows and battery.

### Demo navigation

| Gesture | Weekly view | Daily view |
| ------- | ----------- | ---------- |
| Tap left arrow (bottom header) | Previous day range | Previous day |
| Tap right arrow (bottom header) | Next day range | Next day |
| Swipe left/right in bottom header | Next / previous range | Next / previous day |
| Tap a column | Open daily view for that day | — |
| Tap `< Week` button | — | Return to weekly |
| Button press (GPIO 21) | Cycle to next demo screen | Cycle to next demo screen |

Swipe detection: 400 ms max duration, 40 px min travel, 450 ms cooldown.

---

## Dashboard target (`pio run -e dashboard`)

The full calendar dashboard firmware. Entry point is
`src/main_dashboard.cpp` (restored from the original backup).

### Sleep/wake cycle

1. **Cold boot** — connect WiFi, sync SNTP, connect MQTT, wait for retained
   payload, render.
2. **Timer wake** — refresh WiFi/MQTT, render latest data, go back to sleep.
3. **Button/touch wake** — replay cached payload from RTC RAM, stay awake for
   interaction, sleep after inactivity.

### Active hours

- **7 AM – 10 PM**: stays awake for 5 minutes after user interaction, then
  sleeps for `SCHEDULED_INTERVAL_MS` (default 2 hours).
- **10 PM – 7 AM**: immediately deep-sleeps until 7 AM.

### Views

#### Weekly overview (5 columns)

Five day columns with proportional mini-timeline from **7 AM to 10 PM**:

- Day name and day number at the top (MeltSwashes14 font).
- Timed events positioned by absolute clock time, with side-by-side lanes for
  overlaps.
- Event blocks use spaced-apart grayscale shades (`{3, 7, 10, 13}`) with
  white-on-dark or black-on-light text.
- All-day events in a bottom black banner; multiple events shown as
  `"EventA, EventB"` or `"EventA +2"`.
- Tap a column to open daily view.
- Bottom-left/right arrows scroll to the previous/next week.

#### Daily detail view

Full-screen view for a single day:

- Header with back arrow, full date (e.g., `"Mon, Jun 15"`), battery meter.
- All-day banner across the top.
- Scrolling list cards: title, time range, location, description, calendar,
  type.
- Bottom arrows scroll by single day (wrapping across weeks at edges).
- Tap back arrow to return to weekly.

### Navigation summary

| Gesture | Weekly view | Daily view |
| ------- | ----------- | ---------- |
| Tap left arrow | Previous week | Previous day |
| Tap right arrow | Next week | Next day |
| Tap column | Open daily view | — |
| Tap back arrow | — | Return to weekly |
| Button press | Toggle to daily | Toggle to weekly |

---

## MQTT model

```
publisher (cron/script/integration)
        │   sets retain=true
        ▼
   MQTT broker stores latest message
        │
        ▼
   ESP32 wakes and subscribes on connect
        │
   CalendarDashboard parses events → renders weekly or daily view
```

**Topic:** `dashboard/calendar` (configurable in `config.h`)

The MQTT buffer is 4 KB in `networking.cpp`. The RTC RAM payload cache is also
4 KB (matched sizes — a payload up to 4 KB will always be cached for
button-wake replay). The publisher **must** set `retain=true`.

### Payload schema

See [`example-payload.json`](example-payload.json):

```jsonc
{
  "updated": "2026-06-11T19:05:00Z",
  "horizon_days": 7,
  "events": [
    {
      "title": "Summer Art Festival",
      "start": "2026-06-12",
      "end": "2026-06-15",
      "all_day": true,
      "location": "",
      "description": "",
      "calendar": "main",
      "type": "event"
    },
    {
      "title": "Chiro @ 3:30",
      "start": "2026-06-15T15:30-05:00",
      "end": "2026-06-15T16:00-05:00",
      "all_day": false,
      "location": "",
      "description": "",
      "calendar": "main",
      "type": "event"
    },
    {
      "title": "MEETING (fake)",
      "start": "2026-06-17T15:20-05:00",
      "end": "2026-06-17T16:20-05:00",
      "all_day": false,
      "location": "",
      "description": "",
      "calendar": "shyft",
      "type": "event"
    }
  ]
}
```

| Field | Meaning |
| ----- | ------- |
| `title` | Event/task title |
| `start` / `end` | ISO datetime or date. Timezone offset is parsed by `sscanf` but effectively discarded — local time is assumed. |
| `all_day` | `true` for all-day/multi-day events (expanded per-day) |
| `location` | Optional, shown in daily list cards |
| `description` | Optional, shown in daily list cards |
| `calendar` | Source calendar name; hashed to a grayscale shade |
| `type` | `"event"` or `"task"` (displayed as metadata) |
| `updated` | Publisher timestamp (**not used by firmware**) |
| `horizon_days` | Intended horizon window (**not used by firmware**) |

Multi-day all-day events are expanded into one entry per calendar day.

---

## Configuration reference

All tunables live in [`include/config.h`](include/config.h).

| Setting | Default | Purpose |
| ------- | ------- | ------- |
| `WIFI_SSID`, `WIFI_PASSWORD` | (from `secrets.h`) | WiFi credentials |
| `MQTT_HOST` | `192.168.1.38` | Broker address |
| `MQTT_PORT` | `1883` | Broker port |
| `MQTT_USER`, `MQTT_PASS` | (from `secrets.h`) | MQTT auth (`nullptr` = anonymous) |
| `MQTT_CLIENT_ID` | `t5-calendar-dashboard` | Client ID |
| `MQTT_TOPIC` | `dashboard/calendar` | Subscription topic |
| `VERSION_TAG` | `v2.7` | Shown in the dashboard header |
| `SCHEDULED_INTERVAL_MS` | `7 200 000` (2 h) | Timer-wake refresh interval |
| `INACTIVITY_TIMEOUT_MS` | `300 000` (5 min) | Sleep after no activity |
| `PAYLOAD_WAIT_MS` | `8000` | Max wait for retained MQTT payload |
| `NTP_SYNC_TIMEOUT_MS` | `10000` | SNTP sync timeout on cold boot |
| `BUTTON_PIN` | `21` | Physical button (RTC-capable) |
| `BATTERY_ADC_PIN` | `14` | Battery voltage divider ADC |
| `BATTERY_SAMPLES` | `16` | ADC oversampling count |
| `BATTERY_DIVIDER` | `2.0f` | Voltage divider ratio |
| `TOUCH_SDA` / `TOUCH_SCL` | `18` / `17` | GT911 I²C bus |
| `TOUCH_INT_PIN` | `47` | GT911 IRQ (non-RTC) |
| `TOUCH_WAKE_PIN` | `10` | RTC-capable GPIO for touch wake (requires GPIO47→GPIO10 bridge) |
| `TIMEZONE` | `CST6CDT,M3.2.0/2,M11.1.0/2` | POSIX TZ string (US Central) |
| `NTP_SERVER_1` | `pool.ntp.org` | Primary NTP server |
| `NTP_SERVER_2` | `time.nist.gov` | Secondary NTP server |
| `HEARTBEAT_MS` | `30000` | Health-log interval |

WiFi credentials and MQTT auth live in [`include/secrets.h`](include/secrets.h)
(gitignored). Copy [`include/secrets.example.h`](include/secrets.example.h) →
`include/secrets.h` and fill in values before flashing the dashboard target.

---

## File layout

```
project-root/
├── platformio.ini               ← two envs: demo (default) + dashboard
├── boards/
│   └── T5-ePaper-S3.json        ← custom board definition (16 MB QIO flash)
├── convert_fonts.sh             ← Linux font conversion script
├── convert_fonts.bat            ← Windows font conversion script (legacy)
├── fonts_tff/                   ← source TTF files
│   ├── GentyDemo-Regular.ttf
│   ├── Melt-Swashes.ttf
│   ├── DS-DIGI.TTF
│   ├── Computerfont.ttf
│   └── abduction2002.ttf
├── example-payload.json         ← reference MQTT payload (3 events)
├── ui.txt                       ← UI design notes / wireframe sketches
├── CURRENT_STATE.md             ← this file
├── README.md                    ← STALE — pending rewrite
├── include/
│   ├── config.h                 ← tunables, pins, network, timezone
│   ├── secrets.h                ← WiFi + MQTT credentials (gitignored)
│   ├── secrets.example.h        ← template
│   ├── dashboard.h              ← abstract Dashboard base class
│   ├── ui.h                     ← screen enum + rendering API
│   ├── battery.h
│   ├── display_manager.h
│   ├── networking.h             ← WiFi/MQTT API (dashboard target only)
│   ├── power_mgr.h
│   ├── touch_input.h
│   ├── fonts/                   ← converted GFXfont headers
│   │   ├── Genty16pt7b.h        ← used by ui.cpp
│   │   ├── Genty20pt7b.h        ← used by ui.cpp
│   │   ├── Genty24pt7b.h        ← used by ui.cpp
│   │   ├── Genty32pt7b.h        ← used by ui.cpp
│   │   ├── Genty48pt7b.h        ← used by ui.cpp
│   │   ├── Melt-Swashes14pt7b.h ← used by ui.cpp
│   │   └── Melt-Swashes16pt7b.h ← used by ui.cpp
│   └── dashboards/
│       └── calendar_dashboard.h ← CalendarEvent struct + CalendarDashboard class
└── src/
    ├── main.cpp                 ← demo entry point (demo env)
    ├── main_dashboard.cpp       ← dashboard entry point (dashboard env)
    ├── ui.cpp                 ← weekly/daily screen rendering + touch gestures
    ├── display_manager.cpp      ← framebuffer alloc, power, refresh, splash
    ├── networking.cpp           ← WiFi/MQTT/SNTP/RTC cache (dashboard env)
    ├── power_mgr.cpp            ← wake reason + deep sleep config
    ├── touch_input.cpp          ← GT911 I²C driver, polled at 150 ms
    ├── battery.cpp              ← ADC sampling, LiPo curve → percent
    └── dashboards/
        └── calendar_dashboard.cpp ← JSON parse only
```

---

## Module rundown

| Module | Demo | Dashboard | Notes |
| ------ | :--: | :-------: | ----- |
| `main.cpp` | ✓ | — | Demo entry: boots display/touch/battery, feeds hardcoded events to `ui.cpp`. |
| `main_dashboard.cpp` | — | ✓ | Dashboard entry: wake routing, MQTT fetch, interactive loop, sleep schedule. |
| `ui.cpp` | ✓ | ✓ | Weekly (4-col) + daily views, touch gestures. Data supplied by caller. |
| `dashboards/calendar_dashboard.cpp` | — | ✓ | JSON parser; exposes parsed events to `ui.cpp` for rendering. |
| `networking.cpp` | — | ✓ | WiFi, MQTT, SNTP, RTC-RAM payload cache (4 KB). |
| `display_manager.cpp` | ✓ | ✓ | PSRAM framebuffer, `epd_init`, `fullRefresh`, `drawSplash`. |
| `touch_input.cpp` | ✓ | ✓ | GT911 I²C, auto-detect address, 150 ms poll, panel coordinate mapping. |
| `battery.cpp` | ✓ | ✓ | 16-sample ADC, piecewise-linear LiPo curve, RTC RAM cached. |
| `power_mgr.cpp` | ✓ | ✓ | Wake reason detection, timer + ext1 button wake config. |

---

## Build & flash

```bash
pio run -e demo                  # compile demo (default)
pio run -e demo -t upload        # compile + flash demo
pio run -e dashboard             # compile dashboard
pio run -e dashboard -t upload   # compile + flash dashboard
pio device monitor               # 115200 baud serial monitor
```

Serial ports are auto-detected (no hardcoded port in `platformio.ini`). On
Linux the device is typically `/dev/ttyUSB0` or `/dev/ttyACM0`.

### Font conversion

```bash
./convert_fonts.sh               # regenerate GFXfont headers from TTFs
```

Requires Python + `freetype-py` (`pip install freetype-py`). Run `pio run`
first so PlatformIO downloads the LilyGo-EPD47 converter script.

Dependencies (`lib_deps` in `platformio.ini`):
- `knolleary/PubSubClient@^2.8` — MQTT client (dashboard only)
- `bblanchon/ArduinoJson@^7.0.4` — JSON parsing (dashboard only)
- `LilyGo-EPD47` (esp32s3 branch) — e-paper display driver (both)
- `lewisxhe/SensorLib@^0.3.1` — GT911 touch driver (both)

---

## Diagnostics

Serial output at 115200 baud:

- **Demo:** boot info (wake reason, free heap), `[demo]` render/button lines.
- **Dashboard:** boot info (wake reason, heap, PSRAM, flash, RTC time), WiFi
  connection details + RSSI, MQTT state + payload previews, parsed event/day
  summary, render events, sleep decisions, periodic heartbeat with heap +
  connection status.

---

## Design notes

[`ui.txt`](ui.txt) contains wireframes and notes for both views. Key goals:

- Weekly: 3-day or 5-day columns, title bar with "Coming next: …" and battery,
  all-day banner as black bar at column bottom.
- Daily: tear-off calendar page on the left, event list on the right with 96 px
  rows, alternating gray backgrounds, time in MeltSwashes16, title in Genty20.
- Battery icon: "something a little more stylized."
- Day label: "Today", "Tomorrow", "Friday (+2D)" style.

The demo partially implements these but diverges (4 columns, bottom header
instead of top title bar, 110 px rows). The dashboard target implements the
5-column weekly + daily list but without the tear-off calendar graphic.

---

## Changes made in this audit (June 23, 2026)

### Deleted (dead code)
- `src/tinyfont.cpp` + `include/tinyfont.h` — never included by any file;
  header declared externs that didn't exist.
- `src/graphics/seven_segment.cpp` + `include/graphics/seven_segment.h` —
  never included; README claimed 7-seg day numbers but code used GFXfont.
- `src/graphics/sprite.cpp` + `include/graphics/sprite.h` — never included.
- `include/graphics/` directory — empty after deletions.
- 6 unused font headers: `Genty14pt7b.h`, `Melt-Swashes18pt7b.h`,
  `Melt-Swashes20pt7b.h`, `Melt-Swashes24pt7b.h`, `Melt-Swashes32pt7b.h`,
  `Melt-Swashes48pt7b.h`.

### Renamed
- `src/main.cpp.dashboard-backup` → `src/main_dashboard.cpp` (now a valid
  compilable source file, used by the dashboard env).

### Build config (`platformio.ini`)
- Removed hardcoded `upload_port = COM8` and `monitor_port = COM8` (Windows).
  Ports now auto-detect.
- Split into two PlatformIO environments with `src_filter`:
  - `[env:demo]` — excludes `main_dashboard.cpp`, `networking.cpp`,
    `dashboards/*`.
  - `[env:dashboard]` — excludes `main.cpp` only.
- Shared settings in `[env]` base section.

### Linux support
- Created `convert_fonts.sh` (auto-detects converter path in `.pio/libdeps`,
  forward slashes, `set -euo pipefail`). The old `.bat` is kept for
  cross-platform reference.

### Code quality fixes
- `calendar_dashboard.cpp`: stripped to JSON parsing only; rendering moved to
  `ui.cpp`. Added `sscanf` return-value check in `parseIsoDateTime()`.
- `power_mgr.cpp`: added serial log for unknown ext1 wake GPIO mask before
  falling back to `WAKE_BUTTON`.
- `networking.cpp`: increased `RTC_PAYLOAD_CAP` from 3072 to 4096 to match
  the MQTT buffer size — prevents silent cache misses for payloads between
  3–4 KB.
- `ui.cpp`: added status comment documenting the RTL text experiment as
  experimental.

---

## Remaining items for future work

- **`README.md` is stale** — it still describes the old single-target
  dashboard firmware. Should be rewritten or replaced with a pointer to this
  file.
- **`horizon_days` and `updated` payload fields** are parsed by the JSON
  deserializer but never read by the firmware. Either use them or remove from
  the example payload.
- **Bubble sorts** (O(n²)) in `ui.cpp` — arrays are small so performance is
  fine, but `qsort` would be cleaner.
- **Magic numbers** in rendering code — pixel offsets and character-width
  estimates are hardcoded inline throughout both `ui.cpp` and
  `calendar_dashboard.cpp`.
- **Touch poll throttling** — `touch_input.cpp` polls at 150 ms intervals;
  `ui.cpp` swipe detection uses a 400 ms window. This can cause missed
  swipes if the first poll lands late in the window. Consider reducing poll
  interval or switching to interrupt-driven touch.
- **Stale `.pio/` build artifacts** — run `rm -rf .pio` and rebuild to clear
  leftover dependencies from old board configurations.
