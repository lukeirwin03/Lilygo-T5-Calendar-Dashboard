# T5 Calendar Dashboard

Firmware for a **LilyGo T5 4.7" e-paper (ESP32-S3)** that subscribes to a calendar MQTT topic and renders a **focus+context weekly view** plus a **daily list/detail view** with touch navigation. The device spends most of its time in deep sleep and wakes on a timer, button press, or touch.

## Hardware

| Part           | Detail                                                |
| -------------- | ----------------------------------------------------- |
| Board          | LilyGo T5 4.7" e-paper V2.3 (ESP32-S3)                |
| Panel          | 960 × 540 4-bit grayscale e-paper                     |
| Display driver | `LilyGo-EPD47` (`epd_driver.h`)                       |
| Touch          | GT911 capacitive touch (I²C)                          |
| User button    | GPIO 21 (active-low, RTC-capable)                     |
| SD card pins   | MISO=16, MOSI=15, SCK=11, CS=42                       |

All pins and network constants live in [`include/config.h`](include/config.h).

## Build environments

| Env | Purpose | Entry point | WiFi/MQTT | SD card |
| --- | ------- | ----------- | --------- | ------- |
| `demo` | UI prototyping with hardcoded test events | `src/main.cpp` | No | Optional |
| `dashboard` | Production firmware | `src/main_dashboard.cpp` | Yes | Yes |

The demo build is the photo/manual-test build: it plays the connection-screen animation at boot (touch or button skips it), runs a **simulated clock** starting at 9:42 AM on the boot date, and **long-pressing the button jumps the clock +2h** (wrapping past midnight back to 6 AM) — so every sliding-window state (morning ▲ clamp, mid-day slide, freeze at the last event, evening ▼ clamp) and the scroll arrows can be walked through and photographed without waiting for real time to pass.

```bash
pio run -e demo -t upload          # flash demo (no WiFi needed)
pio run -e dashboard -t upload     # flash production
pio device monitor                 # 115200 baud serial monitor
```

## Views

### Weekly view (default)

A 3-column **focus+context** layout:

- **Left context** (~224px): chronological text list of the previous day's events (time range + title per row, alternating stripes)
- **Focus column** (~448px, center): proportional event blocks on a **dynamic timeline** that snaps to a nice duration (3h, 6h, 9h, 12h, 18h, or 24h) based on the day's events
- **Right context** (~224px): chronological text list of the next day's events

Focus column features:
- Event blocks sized proportionally to duration, with lane splitting for overlapping events
- **Today (clock set): a sliding timeline window.** Anchored at the **Day Start** setting, it follows *now − 1h* as the day progresses (each wake's re-render slides it forward), always extends past the next upcoming event (minimum 6 hours), and **freezes at the last event** — so the last event stays pinned at the top of the window for the rest of the day. Past events scroll up off the top; bring them back with the scroll-up arrow
- **Other days (or unset clock): a deterministic event window** — 1 hour before the earliest event to 1 hour after the latest, snapped to the next nice duration (6h, 9h, 12h, 18h, or 24h), clamped to 4 AM–4 AM. Identical on every render
- Boundary lines at the top and bottom show the time range (e.g., "8:00 AM" and "7:00 PM")
- Two grid lines at 1/3 and 2/3 of the timeline
- Faint gray fill in gaps between events (>60 min)
- Time range shown at the top of each block (e.g., "2:00 - 6:00 PM")
- All-day events shown in a black banner above the timeline (with ← / → continuation arrows for multi-day events)
- **Now marker** — two small chevrons pointing at each other ('> · <', white-haloed) near the column edges at the current time on today's focus column. When "now" is outside the window the pair clamps just inside the boundary with an extra chevron pointing at the off-screen direction (▲ early morning, ▼ after the frozen end) — the marker never lies about where "now" is
- **Scroll arrows** — small chevron buttons (24 px visuals, much larger tap zones) appear at the top-right/bottom-right of today's timeline only when events are clipped off that edge. Tap ▲ to page back to scrolled-off past events, ▼ to return toward the live window; the offset resets on the next data refresh or navigation. Scrolling renders with a fast **no-clear ghost refresh** of the focus column — prior timeline frames leave faint ghosts (an intentional experiment in trading ghosting for snappiness); the next full refresh cleans the panel
- At the edge of the ±Context-Days window the adjacent context column is hidden entirely, making the navigation boundary obvious

Context column features:
- Ultra-compact 12-hour time ranges: "9:00-10:00AM" (same half) or "11:30A-1:00P" (crossing AM/PM)
- Alternating LTGRAY/white row stripes
- All-day banner at the top (if any)

### Daily view

Single-day view with a tear-off calendar widget, a 3-button nav container, and an event list:

- **Left column**: 220px tear-off calendar (day name + number) + a **daily nav container** with three buttons: ◀ (previous day), **Back to Week**, and ▶ (next day)
- **Right column**: Compact 2-line event rows (time range + title, 60px each, alternating stripes)
- A gray **status strip** across the top shows the event counts (e.g., "Events: 4 | All-day: 1") and, when viewing today with the clock set, the current time ("Now 3:42 PM")
- **Tap any event row** → switches the right column to a **detail view** showing full event info (title, time range, location, description, calendar source) with a "← Back to list" button
- Detail transitions use **partial refresh** (only the right column refreshes, ~2s instead of ~5s full refresh)

### Navigation

| Action | Weekly view | Daily view |
| ------ | ----------- | ---------- |
| Tap left context column | Previous day | — |
| Tap right context column | Next day | — |
| Tap focus column | Open daily view | — |
| Tap ◀ / ▶ in daily nav | — | Previous / next day |
| Tap event row (daily) | — | Open event detail |
| Tap "← Back to list" | — | Return to event list |
| Tap "Back to Week" (daily nav) | — | Return to weekly view |
| Button press | Toggle settings modal | Toggle settings modal |

> Returning to the weekly view centered on today also happens automatically: on the inactivity sleep timeout the device resets to the weekly-today view before sleeping (replacing the old explicit "Today" jump).

### Settings

Tap the physical button to open the **Settings modal** (tap the button again, or tap **Close** inside the modal, to dismiss). The modal opens as a partial-refresh overlay and shows:

- A **battery readout** in the title bar (glyph + percentage)
- A **Display** tab — Day Start, Day End, Time Format (12h/24h), Context Days (days of context shown each side of today, 1–7)
- A **Power** tab — Refresh Every, Sleep After, Sleep Starts, Sleep Ends, Keep History
- A **Diagnostics** tab — Last Updated (age of the displayed payload, from its `updated` field), WiFi and MQTT (live state on the rare occasion the radio is actually up — normally the outcome of the most recent connection attempt, e.g. "Last: OK" / "Last: fail", kept in RTC memory so it survives deep sleep), Battery, free Memory
- **− / +** buttons cycle the selected row's value; **Save** persists all values to the SD card and closes the modal
- A **Sync** button next to **Save** — forces a fresh MQTT pull (the modal closes; the display re-renders when fresh data lands, ~10–30 s later) (dashboard env only — the demo build has no networking)

**Time Format (12h/24h)** toggles event times between 12-hour (`3:30 PM`) and 24-hour (`15:30`) display across the weekly and daily views.

**Sleep After** is the inactivity timeout (default 1 minute): once the device has been idle for that long it resets the display to the weekly-today view, renders it, and goes to deep sleep. This timeout also gates the nightly window — active use keeps the device awake even after the window opens.

**Sleep Starts / Sleep Ends** define the nightly deep-sleep window (default 10 PM – 7 AM). During this window the device sleeps straight through until the end hour — timer wakes that land inside the window skip the WiFi refresh entirely and go back to sleep. Events that fall inside the window do **not** trigger a wake. This scheduled window applies to the **dashboard** env only — the **demo** env runs a simulated clock and ignores the window, sleeping on the **Sleep After** inactivity timeout.

Changes are persisted to `/config/settings.json` on the SD card when a card is present; otherwise the defaults are used for the session.

## Sleep / wake cycle

The device spends almost all its time in deep sleep (~10–150 µA) and wakes on three triggers:

1. **Cold boot** — load cached payload from SD and render immediately (if the RTC clock is valid), then connect WiFi/MQTT in the background and re-render if the broker delivers a fresh payload. If there is no usable cache (first boot, or power loss with the clock unset), a stylized **connection screen** takes over: a wordmark, a progress bar, and a live stage label ("Connecting to WiFi… → Syncing clock… → Connecting to broker… → Fetching calendar…") updated with flash-free **differential refreshes** of just the status band (old text erased with white-ink, new text inked — ~half a second per update, no panel flashing; the progress bar is append-only — its existing fill is never re-driven, new progress is simply drawn in), with failure labels ("WiFi unavailable", "Broker unreachable", …) when a stage fails.
2. **Timer wake** — refresh WiFi/MQTT, render latest data, stay touch-responsive for at least 15 s, go back to sleep
3. **Button/touch wake** — show the cached payload instantly, then connect and pull the latest retained message in the background (fresh data re-renders ~10–30 s later); sleep after inactivity

### How long it sleeps (dashboard env)

Sleep duration is the next periodic boundary, adjusted for the nightly window:

- **Periodic wake** — the device wakes at the **Refresh Every** interval (default 1 hour), aligned to the top of the hour — just past :00, or on the interval boundary for sub-hour settings. This is the freshness guarantee: any event added to the MQTT source while the device sleeps is discovered within one interval.
- **Sleep window** — during the nightly window (default 10 PM – 7 AM) the device skips all of the above and sleeps straight through to morning. Timer wakes that land inside the window detect it from the RTC and go back to sleep without turning on WiFi. Events during the window do not trigger a wake.

If the nightly window opens before the next planned wake, the device sleeps straight to morning instead of waking at the boundary — avoiding a pointless wake-and-resleep cycle.

Active use overrides the window: the device won't sleep while you're interacting with it. The **Sleep After** inactivity timeout (default 1 minute) gates all sleep, every refresh keeps the device awake (and touch-responsive) for at least 15 s, and any input resets the timeout — so even after the window opens you get your full browsing grace period.

> The **demo** env mirrors the timer/button wake behavior without networking. It runs a simulated clock (no NTP), so it ignores the sleep window — it sleeps on the **Sleep After** inactivity timeout for the flat **Refresh Every** interval. The GPIO47 → GPIO10 touch-wake bridge described below applies to **both** envs.

## Local cache (SD card)

Every MQTT payload is persisted to the SD card so the device works without a network connection and survives power loss:

- **`/cal/current.json`** — the latest raw payload. On cold boot this is loaded and rendered immediately (before WiFi connects), so the calendar appears in ~2 seconds instead of waiting ~30 seconds for a network connection. The background WiFi refresh updates it if the broker delivers a fresh payload. *(Requires a valid RTC clock; on a true power-loss boot the clock starts unset, so the connection screen shows while NTP syncs.)*
- **`/cal/cache/YYYY-MM-DD.json`** — a per-day cache of parsed events. When a fresh payload arrives, every day it covers is written to its own cache file so the recent-past side of the ±Context-Days window stays filled as events age out of the published horizon. On each payload the cached past days are merged back in and **deduplicated** against payload events (same date, title, start time, all-day flag, and calendar = same occurrence), so an event present in both sources renders exactly once. Entries older than 28 days are evicted at boot.
- **`/cal/history/YYYY-MM-DD.jsonl`** — a deduped historical record. A new JSONL entry is appended only when the payload changes (detected via a checksum stored in RTC memory that survives deep sleep). Old entries are pruned by the **Keep History** retention setting (default 365 days).
- **`/logs/YYYY-MM-DD.log`** — serial-style diagnostic logs, pruned by the same retention setting.

The RTC RAM cache (used for instant replay on button wakes) is the fast path; the SD card is the persistent fallback that survives power loss.

## Power management

The device is designed for battery operation — it spends most of its time in deep sleep (~10–150 µA) and wakes only when needed. The e-paper display retains its image with zero current between refreshes.

### Sleep scheduling
- **Periodic wake** — wakes at the **Refresh Every** interval (default 1h), aligned to the top of the hour, to discover newly-added events.
- **Sleep window** — during the nightly window (default 10 PM–7 AM), sleeps straight through to morning. Timer wakes inside the window skip WiFi entirely.
- **Inactivity timeout** — sleeps after **Sleep After** seconds (default 60) of no interaction; every refresh keeps the device awake at least 15 s, and any input resets the timer.

### WiFi
- WiFi is **off during touch interaction** — disconnected after the data fetch and only reconnected when fresh data is forced (button/touch wake or the Settings **Sync** button).
- **Early disconnect** — on timer wakes, WiFi is turned off the instant the MQTT payload arrives, before the ~5s render.
- **Reduced TX power** — limited to 17 dBm (from the default ~20 dBm), reducing peak current during WiFi active periods.

### CPU
- Runs at **160 MHz** (down from the default 240 MHz), cutting active power ~30%. The EPD driver uses its own SPI clock and is unaffected.

### Display
- **E-paper (bistable)** — zero current between refreshes.
- **Partial refresh** for daily↔detail transitions and the settings modal (~2s vs ~5s full).
- Display power cut after every refresh cycle.

### Battery
- **Sampled on demand** — voltage read only on cold boot and button wake (when someone might look), not on every timer wake.

### Memory
- **Event window** — events within today ± the **Context Days** setting (default 7, adjustable 1–7 in the Settings modal) are loaded into memory. Events outside the window are still persisted to the SD card history but not held in RAM. Day-navigation is clamped to this range; the weekly context column is hidden and the daily arrow is grayed at the edges to signal the boundary.

### Future opportunities
- **Light sleep during interaction** — replace the touch-poll loop with GPIO-interrupt wake from light sleep (~1–5 mA vs ~80 mA active). Biggest remaining win for battery life.
- **Static IP** — skip DHCP negotiation (~1–2s savings per WiFi connect).
- **Low-battery conservation** — increase sleep intervals when battery is low.

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

The MQTT buffer is 4 KB. The publisher **must** set `retain=true`.
The payload's top-level `updated` timestamp drives the **Last Updated** row in Settings → Diagnostics (shown as data age, e.g. "2h ago") — keep it accurate so staleness is visible there.

See [`docs/mqtt-setup.md`](docs/mqtt-setup.md) for the full broker contract — payload format, field limits, retain behavior, and `mosquitto_pub`/`mosquitto_sub` testing commands.

### Payload schema

See [`example-payload.json`](example-payload.json):

```jsonc
{
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
      "title": "Chiro",
      "start": "2026-06-15T15:30-05:00",
      "end": "2026-06-15T16:00-05:00",
      "all_day": false,
      "location": "West Omaha Chiro",
      "description": "Routine adjustment",
      "calendar": "main",
      "type": "event"
    }
  ]
}
```

Multi-day all-day events are expanded into one entry per calendar day.

## File layout

```
project-root/
├── platformio.ini              ← build config (demo + dashboard envs)
├── boards/
│   └── T5-ePaper-S3.json
├── convert_fonts.sh            ← font pipeline (requires freetype-py)
├── patch_font_renderer.py      ← pre-build script for font renderer patch
├── example-payload.json        ← reference MQTT payload
├── README.md
├── include/
│   ├── config.h                ← pins, network constants, timing
│   ├── secrets.h               ← WiFi + MQTT credentials (gitignored)
│   ├── secrets.example.h       ← template
│   ├── dashboard.h             ← abstract Dashboard base class
│   ├── battery.h
│   ├── display_manager.h       ← framebuffer, full/partial refresh
│   ├── conn_screen.h           ← cold-boot connection screen w/ progress
│   ├── networking.h            ← WiFi, MQTT, RTC payload cache, progress listener
│   ├── power_mgr.h             ← deep sleep + wake reason
│   ├── touch_input.h           ← GT911 touch driver
│   ├── settings.h              ← runtime settings struct (SD card)
│   ├── sd_storage.h            ← SD card: config + logs
│   ├── ui.h                    ← UI namespace: render, touch, screens
│   ├── ui_settings.h           ← settings screen API
│   ├── dashboards/
│   │   └── calendar_dashboard.h  ← event parsing
│   └── fonts/                  ← generated GFXfont headers (Genty, MeltSwashes, Computer)
├── src/
│   ├── main.cpp                ← demo entry point (hardcoded test events)
│   ├── main_dashboard.cpp      ← production entry point (MQTT, SD, sleep)
│   ├── ui.cpp                  ← all rendering + touch state machine (~2000 lines)
│   ├── ui_settings.cpp         ← settings screen rendering + touch
│   ├── conn_screen.cpp         ← connection screen: wordmark, progress bar, partial updates
│   ├── display_manager.cpp     ← PSRAM framebuffer, fullRefresh, partialRefresh
│   ├── networking.cpp          ← WiFi + MQTT lifecycle, SNTP, RTC cache
│   ├── power_mgr.cpp           ← wake reason detection, deep sleep
│   ├── touch_input.cpp         ← GT911 I²C driver
│   ├── battery.cpp             ← ADC sampling, LiPo curve
│   ├── settings.cpp            ← settings load/save from SD card
│   ├── sd_storage.cpp          ← SD card config + log management
│   └── dashboards/
│       └── calendar_dashboard.cpp  ← JSON parsing, event expansion
└── test/
    ├── MANUAL_TESTS.md
    ├── test_calendar/          ← event parsing unit tests
    └── test_settings/          ← settings value cycle tests
```

### Module rundown

- **`ui.cpp`** — All rendering (weekly focus+context, daily list/detail) and the touch state machine. Manages screen state, navigation, partial refresh coordination, and the dynamic timeline.
- **`ui_settings.cpp`** — Settings screen with sidebar navigation, value cycling, and partial refresh for row updates.
- **`main.cpp`** / **`main_dashboard.cpp`** — Entry points for demo and production. Handle wake routing, touch polling, render scheduling, and sleep logic.
- **`conn_screen.cpp`** — Cold-boot connection screen: wordmark + progress bar, stage labels, and throttled partial refreshes of just the status band while connecting.
- **`display_manager.cpp`** — PSRAM framebuffer allocation, full refresh, cleared partial refresh, and no-clear ghost refresh (both draw full-width rows to avoid EPD edge artifacts).
- **`networking.cpp`** — WiFi/MQTT lifecycle, SNTP time sync, RTC-RAM payload cache for offline wake, and an optional progress listener the connection screen subscribes to.
- **`dashboards/calendar_dashboard.cpp`** — JSON event parsing, all-day event expansion, shade assignment. Does NOT render (rendering is in `ui.cpp`).
- **`settings.cpp`** — Runtime settings struct loaded from `/config/settings.json` on SD card.
- **`sd_storage.cpp`** — SD card mount, config file read/write, log management with retention cleanup.

## Configuration

WiFi credentials and MQTT auth live in [`include/secrets.h`](include/secrets.example.h) (gitignored). Copy `secrets.example.h` → `secrets.h` and fill in values before flashing the dashboard env.

Runtime settings (display hours, refresh interval, etc.) are stored on the SD card at `/config/settings.json` and edited through the settings UI.

## Font pipeline

Fonts are generated from `.ttf` files using `convert_fonts.sh`, which requires `freetype-py` (install in a virtualenv):

```bash
python -m venv .venv
source .venv/bin/activate
pip install freetype-py
./convert_fonts.sh
```

The pipeline generates GFXfont headers for:
- **Genty** (24pt, 32pt, 48pt) — decorative headings, day numbers
- **MeltSwashes** (14pt, 16pt, 18pt) — body text, time labels
- **Computer** (14pt, 16pt, 20pt) — monospace (available but not currently used in rendering)

## Notes

- The panel retains its last image with zero current draw between deep sleeps.
- WiFi is disconnected the moment the MQTT payload arrives, before the render — the render and deep sleep don't need network, and WiFi is the single biggest power draw (~120–500 mA active vs. ~10–150 µA in deep sleep).
- Touch wake from deep sleep requires **both** a hardware bridge from GPIO47 (TOUCH_INT) to GPIO10 (TOUCH_WAKE_PIN) — GPIO47 is not RTC-capable — **and** uncommenting the `TOUCH_WAKE_PIN` line in `power_mgr.cpp`'s `sleepFor()` (it ships commented out). Without both, only the button can wake from deep sleep.
- The demo environment is the photograph/manual-test build: simulated clock (9:42 AM start, long-press button = +2h jump), boot-time connection-screen animation, plus touch diagnostics (heartbeat, init-failure logging) and 26 hardcoded test events covering edge cases (overlaps, gaps, all-day, cross-midnight, long titles).
- Partial refresh comes in three flavors. The raw epdiy driver assumes the panel is **white** when drawing (`BLACK_ON_WHITE`, no previous-frame tracking): it can drive pixels toward black/gray, but a **white target produces no drive** — a plain no-clear push can only *add* ink. **Ghost refresh** (`ghostRefresh`, timeline scrolling): exactly that — fast and flash-free, but old content stays at full strength. **Cleared partial refresh** (daily↔detail, settings modal): `epd_clear_area` inverts the region dark-then-white before drawing — the brief flash *is* the erase. **Differential refresh** (`diffRefresh`, connection screen): flash-free full-swap — the app tracks what ink is physically on the panel, then per update: one `WHITE_ON_WHITE` "white ink" pass erases *all* the old ink (glyph-masked — paper/background pixels are never driven), no settle gap, and two fast 1-bit passes ink all the new content (~half a second per update, no flashing). Hardware-characterized on this panel via the demo build's calibration screen: a single erase pass is the correct dose (more passes build charge and the erased pixels fade back in), whole-band erases grey the background, and gap-0 two-pass-ink swaps are clean. This gets Kindle-like silent updates out of dumb glass — the driver has the white-ink mode, it just has no memory of what was drawn, so the app supplies it.
