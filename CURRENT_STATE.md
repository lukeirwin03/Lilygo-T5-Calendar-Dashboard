# T5 Calendar Dashboard — Current State

> **Last updated:** July 22, 2026
> **Purpose:** Accurate snapshot of the codebase after the UI redesign (commit `cc98492`).

---

## TL;DR

The project has **two build targets** selectable via PlatformIO environments:

| Target | Command | What it does |
|---|---|---|
| **demo** | `pio run -e demo` | UI development with 26 hardcoded test events covering edge cases. No WiFi, no MQTT, no sleep. Touch diagnostics included. |
| **dashboard** | `pio run -e dashboard` | Full production firmware: WiFi, MQTT subscribe, SNTP, deep sleep, touch/button wake, RTC cache, SD card logging, settings. |

Both share the same rendering code (`ui.cpp`), font pipeline, and display/touch/battery/power modules.

---

## Hardware

| Part | Detail |
|---|---|
| Board | LilyGo T5 4.7" e-paper V2.3 (ESP32-S3) |
| Panel | 960 × 540 4-bit grayscale e-paper |
| Display driver | `LilyGo-EPD47` (`epd_driver.h`) |
| Touch | GT911 capacitive touch (I²C, SDA=18, SCL=17) |
| User button | GPIO 21 (active-low, RTC-capable) |
| Battery ADC | GPIO 14 (voltage divider, ratio 2.0×) |
| SD card pins | MISO=16, MOSI=15, SCK=11, CS=42 |

Board definition: `boards/T5-ePaper-S3.json` (16 MB QIO flash, OPI PSRAM, ESP32-S3).

---

## Weekly View — Focus + Context Layout

The weekly view uses a **focus + context** design with 3 columns:

```
┌──────────┬──────────────────────┬──────────┐
│ context  │                      │ context  │
│ (prev    │    FOCUS COLUMN      │ (next    │
│  day)    │    (2× width,        │  day)    │
│          │     extends to       │          │
│          │     bottom screen)   │          │
├──────────┤                      │          │
│ ← arrow  │                      │          │
│ Today    │                      ├──────────┤
│ button   │                      │ arrow →  │
│          │                      │ battery  │
└──────────┴──────────────────────┴──────────┘
```

### Navigation
- **Arrow taps**: ±1 day per tap
- **Context column tap**: navigate focus to that day (±1 day)
- **Focus column tap**: open daily view for that day
- **"Today" button**: jump focus back to today
- **Swipe**: removed (simplified to arrows only)
- **Long-press battery icon**: toggle debug mode (reveals Settings gear)

### Event rendering
- **Short events (≤60 min)**: 1-line format `"3:30PM Title..."`, proportional height (28–50 px)
- **Regular events (>60 min)**: 2-line format (time on top, title below), min height 76 px
- **Long events (≥3 hr)**: capped at consistent height; show end time at bottom
- **Overlapping events**: lane splitting in focus column (side-by-side blocks); stacked in context columns
- **Back-to-back events**: 4 px visual gap between adjacent blocks
- **White text on dark fills**: 1 px black outline (8-direction halo)

### All-day events
- Rendered as a 36 px black banner at the **top of the timeline**
- Multi-day events show continuation arrows: `← Title` (from yesterday), `Title →` (to tomorrow)
- Multiple all-day events on same day: combined into one banner with `", "` separator

### Indicators
- **Empty day**: centered `"No events"` in gray
- **Time gaps >1 hr**: dotted horizontal line between events
- **Events before 7 AM**: `↑ N before 7AM` indicator at top of timeline
- **Events after 10 PM**: `↓ N after 10PM` indicator at bottom of timeline

### Typography
| Role | Font | Size |
|---|---|---|
| Focus column day name | Genty | 24 pt (full: "Thursday") |
| Context column day name | Genty | 24 pt (short: "Thu") |
| Event time | MeltSwashes | 18 pt |
| Event title (2-line) | MeltSwashes | 16 pt |
| Event title (1-line short) | MeltSwashes | 16 pt |
| Location line | MeltSwashes | 14 pt |
| Indicators, labels | MeltSwashes | 14 pt |

---

## Daily View

Unchanged from the pre-redesign state. Shows a single day with:
- Tear-off calendar page (Genty24 day name, Genty48 day number)
- Event list with time range, title, location, description
- Back button to return to weekly view

**Note**: Daily view typography has NOT been updated to match the weekly view's Genty24/MeltSwashes18 system. This is a future task.

---

## Settings System

Runtime settings loaded from `/config/settings.json` on the SD card at boot. Overrides compile-time defaults from `config.h`.

### Access
Settings screen is **debug-gated**: long-press the battery icon (≥2 seconds) to toggle debug mode. When enabled, a gear icon appears in the right footer. Tap the gear to enter Settings.

### Categories
- **Display**: Day Start hour, Day End hour (num_columns removed — always 3)
- **Power**: Refresh interval, Sleep timeout, History retention
- **Network**: WiFi SSID, MQTT host, MQTT topic (display only — changes require next wake to take effect)

### Wired-up settings
All display and power settings are actually applied:
- `day_start_hour` / `day_end_hour` → timeline range
- `refresh_interval_s` → sleep duration
- `inactivity_timeout_s` → idle sleep trigger
- `history_retention_d` → log cleanup

Network settings (SSID, host, topic) are displayed but NOT yet applied to the running connection — they require a reboot.

---

## Font Pipeline

`convert_fonts.sh` converts TTF files to LilyGo-EPD47 GFXfont headers:
- Atomic writes (tmp + mv) — no half-written headers on failure
- Pre-flight existence checks for TTFs and freetype-py
- Pragma wrappers on all headers to suppress narrowing warnings
- Idempotent — safe to re-run

### Converted fonts
| Font | Sizes | Used for |
|---|---|---|
| Genty | 20, 24, 32, 48 pt | Headings, daily tear-off |
| MeltSwashes | 14, 16, 18, 20 pt | Body text, event details |
| Computer | 14, 16, 20 pt | Settings UI only |

### Evaluated and rejected
- CapitolCity, Platinum Sign Over/Under, DS-DIGI — evaluated via font swatch, rejected on hardware

---

## Power Management

- **Deep sleep**: timer + button wake (ext1). Touch wake requires hardware bridge (GPIO47→GPIO10).
- **Light sleep**: `power_mgr::lightSleep()` and `enableTouchWake()` are **scaffolded but unused**. Wiring them into the main loop would drop idle power from ~80 mA to ~0.8 mA. (Future task.)
- **Active hours**: 7 AM – 10 PM. Outside this window, device deep-sleeps until 7 AM.

---

## SD Card Storage

- **Event caching**: events saved per-day as `/cal/YYYY-MM-DD.json`
- **Settings**: `/config/settings.json`
- **Logging**: `/logs/YYYY-MM-DD.log` (auto-cleans files older than retention period)
- All functions gracefully no-op if the card is not mounted.

---

## Touch Handling

- GT911 capacitive touch via I²C (SensorLib)
- Polled at 10 ms intervals in the main loop
- INT pin checked before I²C read (avoids unnecessary bus traffic)
- State machine: IDLE → DOWN → LIFT_PENDING → classify (tap vs swipe vs long-press)
- Sample-gap recovery: 80 ms lift timeout absorbs GT911's ~16 ms sample gaps
- Diagnostic heartbeat in demo env: `[heartbeat] touch online=? INT=? heap=?` every 5 seconds

---

## File Layout

```
project-root/
├── platformio.ini               ← two envs: demo + dashboard
├── boards/T5-ePaper-S3.json     ← custom board definition
├── convert_fonts.sh             ← font conversion pipeline
├── UI_REDESIGN.md               ← UI redesign plan + testing checklist
├── CURRENT_STATE.md             ← this file
├── PLAN.md                      ← completed power/SD/settings plan (historical)
├── README.md                    ← stale — pending rewrite
├── patch_font_renderer.py       ← auto-patches LilyGo font.c (skip transparent pixels)
├── fonts_tff/                   ← source TTF files (gitignored)
├── include/
│   ├── config.h                 ← compile-time tunables (pins, network, timezone)
│   ├── settings.h               ← runtime settings struct
│   ├── sd_storage.h
│   ├── dashboard.h              ← abstract Dashboard base class
│   ├── display_manager.h
│   ├── networking.h             ← WiFi/MQTT API (dashboard env only)
│   ├── power_mgr.h
│   ├── touch_input.h
│   ├── battery.h
│   ├── ui.h                     ← Screen enum + rendering API
│   ├── ui_settings.h            ← Settings screen API
│   └── fonts/                   ← converted GFXfont headers (Genty, MeltSwashes, Computer)
└── src/
    ├── main.cpp                 ← demo entry (26 test events, touch diagnostics)
    ├── main_dashboard.cpp       ← production entry (MQTT, sleep, SD card)
    ├── ui.cpp                   ← weekly + daily rendering, touch state machine
    ├── ui_settings.cpp          ← settings screen rendering
    ├── settings.cpp             ← settings load/save
    ├── sd_storage.cpp           ← SD card read/write
    ├── display_manager.cpp      ← framebuffer, refresh, splash
    ├── networking.cpp           ← WiFi, MQTT, SNTP, RTC cache
    ├── power_mgr.cpp            ← deep sleep, light sleep (unused)
    ├── touch_input.cpp          ← GT911 driver
    ├── battery.cpp              ← ADC sampling, LiPo curve
    └── dashboards/
        └── calendar_dashboard.cpp ← JSON event parser
```

---

## Build & Flash

```bash
pio run -e demo                  # compile demo
pio run -e demo -t upload        # compile + flash demo
pio run -e dashboard             # compile production
pio run -e dashboard -t upload   # compile + flash production
pio device monitor               # serial monitor (115200 baud)
pio test -e native               # run unit tests (22 tests across 3 suites)
```

Serial ports are auto-detected. On Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`.

---

## Known Limitations

1. **Daily view typography** hasn't been updated to match the weekly view.
2. **Light sleep** is scaffolded but not wired into the main loop.
3. **Network settings** (SSID, MQTT host) are displayed in Settings but not applied to the running connection.
4. **Touch diagnostics** only exist in the demo env, not dashboard.
5. **README.md** is stale and needs a full rewrite.
6. **Per-column hour range compression** — each column shows the full 7 AM–10 PM range regardless of when events actually are.
