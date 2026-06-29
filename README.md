# T5 Calendar Dashboard

Firmware for a **LilyGo T5 4.7" e-paper (ESP32-S3)** that subscribes to a calendar MQTT topic and renders a **5-day weekly overview** plus a **full-screen daily detail view**. The device spends most of its time in deep sleep and wakes on a timer, a button press, or (with a hardware bridge) touch.

## Hardware

| Part           | Detail                                                |
| -------------- | ----------------------------------------------------- |
| Board          | LilyGo T5 4.7" e-paper V2.3 (ESP32-S3)                |
| Panel          | 960 × 540 4-bit grayscale e-paper                     |
| Display driver | `LilyGo-EPD47` (`epd_driver.h`)                       |
| Touch          | GT911 capacitive touch (I²C)                          |
| User button    | GPIO 21 (active-low, RTC-capable)                     |
| SD card pins   | MISO=16, MOSI=15, SCK=11, CS=42                       |

All tunable pins and network settings live in [`include/config.h`](include/config.h). SD card pins are documented here but only used once Phase 6 caching is enabled.

## What it does

The dashboard is a pure **MQTT subscriber** that receives a retained JSON calendar payload, parses it into days/events, and renders one of two views.

### Sleep/wake cycle

1. **Cold boot** — connect WiFi, sync SNTP, connect MQTT, wait for retained payload, render.
2. **Timer wake** — refresh WiFi/MQTT, render latest data, go back to sleep.
3. **Button/touch wake** — replay the cached payload, stay awake for interaction, sleep after inactivity.

### Active hours

- **7 AM – 10 PM**: device stays awake for 5 minutes after user interaction, then sleeps for `SCHEDULED_INTERVAL_MS` (default 2 hours).
- **10 PM – 7 AM**: device immediately deep-sleeps until 7 AM.

### Views

#### Weekly overview (default)

Five day columns showing a proportional mini-timeline from **7 AM to 10 PM**:

- Day name and large seven-segment day number at the top.
- Timed events positioned by absolute clock time, with side-by-side lanes when they overlap.
- Event blocks use spaced-apart grayscale shades (`{3, 7, 10, 13}`) and white-on-dark text.
- All-day events collapsed into a bottom black banner; multiple events shown as `"EventA, EventB"` or `"EventA +2"`.
- Tap a column to open that day in daily view.
- Bottom-left/right arrows scroll to the previous/next week.

#### Daily detail view

Full-screen view for a single day:

- Header with back arrow, full date (e.g., `"Mon, Jun 15"`), and battery meter.
- All-day banner across the top.
- Scrolling list cards showing title, time range, location, description, calendar, and type.
- Bottom arrows scroll by single day (wrapping across weeks at the edges).
- Tap the back arrow to return to weekly view.

### Navigation summary

| Gesture | Weekly view | Daily view |
| ------- | ----------- | ---------- |
| Tap left arrow | Previous week | Previous day |
| Tap right arrow | Next week | Next day |
| Tap column | Open daily view | — |
| Tap back arrow | — | Return to weekly |
| Button press | Toggle to daily | Toggle to weekly |

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

**Topic:** `dashboard/calendar`

The MQTT buffer is set to 4 KB in `networking.cpp`. The publisher **must** set `retain=true` so the broker hands the latest payload to the ESP32 immediately on connect.

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
    }
  ]
}
```

| Field | Meaning |
| ----- | ------- |
| `title` | Event/task title |
| `start` / `end` | ISO datetime or date. Timezone offset is parsed but currently discarded (local time is assumed). |
| `all_day` | `true` for all-day/multi-day events |
| `location` | Optional location shown in daily list cards |
| `description` | Optional description shown in daily list cards |
| `calendar` | Source calendar name; drives the grayscale shade |
| `type` | `"event"` or `"task"` (displayed as metadata) |

Multi-day all-day events are expanded into one entry per calendar day.

## Configuration

All tunables live in [`include/config.h`](include/config.h).

| Setting | Default | Purpose |
| ------- | ------- | ------- |
| `MQTT_HOST`, `MQTT_PORT` | `192.168.1.38`, `1883` | Broker address |
| `MQTT_CLIENT_ID` | `t5-calendar-dashboard` | Client ID reported to broker |
| `SCHEDULED_INTERVAL_MS` | `2 * 60 * 60 * 1000` | Timer-wake refresh interval |
| `INACTIVITY_TIMEOUT_MS` | `5 * 60 * 1000` | Sleep after no touch/button activity |
| `PAYLOAD_WAIT_MS` | `8000` | Max time to wait for retained payload |
| `NTP_SYNC_TIMEOUT_MS` | `10000` | SNTP sync timeout on cold boot |
| `BUTTON_PIN` | `21` | Physical wake/toggle button |
| `TOUCH_INT_PIN` | `47` | GT911 IRQ (non-RTC) |
| `TOUCH_WAKE_PIN` | `10` | RTC-capable GPIO for touch wake (requires GPIO47→GPIO10 bridge) |
| `BATTERY_ADC_PIN` | `14` | Battery voltage divider |

WiFi credentials and MQTT auth live in [`include/secrets.h`](include/secrets.h) (gitignored). Copy [`include/secrets.example.h`](include/secrets.example.h) → `include/secrets.h` and fill in your values before flashing.

## File layout

```
project-root/
├── platformio.ini
├── boards/
│   └── T5-ePaper-S3.json
├── example-payload.json       ← reference MQTT payload
├── plan.md                    ← development plan / status
├── README.md                  ← this file
├── include/
│   ├── config.h               ← tunables, pins, network
│   ├── secrets.h              ← WiFi + MQTT credentials (gitignored)
│   ├── secrets.example.h      ← template
│   ├── dashboard.h            ← abstract Dashboard base class
│   ├── battery.h
│   ├── display_manager.h
│   ├── networking.h
│   ├── power_mgr.h
│   ├── touch_input.h
│   ├── tinyfont.h             ← 5×7 bitmap font
│   └── dashboards/
│       └── calendar_dashboard.h
└── src/
    ├── main.cpp               ← entry point, wake routing, loop, sleep logic
    ├── battery.cpp
    ├── display_manager.cpp    ← framebuffer, power, refresh
    ├── networking.cpp         ← WiFi, MQTT, RTC payload cache
    ├── power_mgr.cpp          ← deep sleep + wake reason helpers
    ├── touch_input.cpp        ← GT911 touch driver
    ├── tinyfont.cpp           ← small font blitter
    └── dashboards/
        └── calendar_dashboard.cpp   ← parsing + weekly/daily rendering
```

### Module rundown

- **`main.cpp`** — `setup()` branches on wake reason: cold boot pulls fresh data, timer wake refreshes and sleeps immediately, button/touch wake replays cached data and enters the interactive loop. `loop()` polls touch, handles the button, renders when dirty, and enforces the active-hours sleep schedule.
- **`networking.cpp`** — WiFi + MQTT lifecycle, SNTP sync, `pumpForPayload()` for the retained message, and an RTC-RAM payload cache (`replayCachedPayload()`) so button/touch wakes do not need the radio.
- **`dashboards/calendar_dashboard.cpp`** — parses the JSON events array, expands all-day events across days, sorts timed events, and renders the weekly overview or daily detail view.
- **`display_manager.cpp`** — allocates the 4-bit grayscale framebuffer in PSRAM, initializes the panel, and provides `fullRefresh()` / `powerOn()` / `powerOff()`.
- **`touch_input.cpp`** — initializes the GT911 over I²C, maps coordinates to the 960×540 panel, and puts the controller to sleep before deep sleep.
- **`battery.cpp`** — samples the battery voltage divider and maps it to a 0–100% estimate stored in RTC RAM.
- **`power_mgr.cpp`** — detects wake reason and configures timer + `ext1` button wake (touch wake is disabled by default because GPIO47 is not RTC-capable).

## Build & flash

```bash
pio run                    # compile
pio run -t upload          # compile + flash
pio device monitor         # 115200 baud serial monitor
```

`platformio.ini` pins the upload/monitor port to **COM8**; change it if your machine assigns a different port. If sprites or fonts look stale after a layout change, run `pio run -t clean` first.

## Diagnostics

Serial output at 115200 baud shows:

- Boot info — wake reason, free heap, PSRAM, flash size, RTC time
- WiFi connection details and RSSI
- MQTT connection state, subscribe status, payload previews
- Parsed event/day summary
- Render events and sleep decisions
- Periodic heartbeat with free heap and connection status

## Roadmap / current status

See [`plan.md`](plan.md) for the full development plan. As of the latest commit:

- ✅ Phases 1–5 complete (hardware boot, MQTT, parsing, dual views, touch/button nav, sleep schedule)
- ⬜ Phase 6 in progress — SD card caching for offline cold-boot support

## Notes

- The panel keeps its last rendered image with zero current draw, so the screen stays visible between deep sleeps.
- Touch wake requires a hardware bridge from `TOUCH_INT_PIN` (GPIO47) to `TOUCH_WAKE_PIN` (GPIO10) because GPIO47 is not RTC-capable on the ESP32-S3. Until that bridge is installed, only the button can wake the device from deep sleep.
