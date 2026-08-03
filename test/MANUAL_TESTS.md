# Manual Hardware Tests

Run these on the actual device after flashing.

## Boot (dashboard env)
- [ ] Device shows "Connecting..." splash on cold boot
- [ ] WiFi connects (check serial: `[wifi] Connected`)
- [ ] MQTT connects and receives payload (check serial: `[mqtt] Connected successfully!`)
- [ ] Calendar renders with events
- [ ] SD card mounts (check serial)

## Weekly view — focus+context layout
- [ ] Three columns visible: left context (text list), focus (event blocks), right context (text list)
- [ ] Focus column has start/end time labels on boundary lines
- [ ] Focus column has two grid lines at 1/3 and 2/3
- [ ] Event blocks are proportional to duration
- [ ] Overlapping events are lane-split (side by side)
- [ ] Gaps between events show faint gray fill
- [ ] All-day events show in a black banner above the timeline
- [ ] Empty days show "No events" placeholder
- [ ] Timeline duration snaps to a nice value (3h, 6h, 9h, 12h, 18h, or 24h)

## Weekly view — context columns
- [ ] Left context shows previous day's events as text list (time + title)
- [ ] Right context shows next day's events as text list
- [ ] Time format: "9:00-10:00AM" (same half) or "11:30A-1:00P" (crossing AM/PM)
- [ ] Alternating row stripes (white/LTGRAY)
- [ ] Long event lists show "+N more..." at the bottom

## Navigation
- [ ] Tap right footer arrow → advances 1 day
- [ ] Tap left footer arrow → goes back 1 day
- [ ] Tap left context column → goes back 1 day
- [ ] Tap right context column → advances 1 day
- [ ] Tap focus column → opens daily view
- [ ] Tap "Today" button → jumps to today
- [ ] No duplicate navigation (touch release gate works)
- [ ] Tapping during render is consumed (no stale tap after refresh)

## Daily view
- [ ] Tear-off calendar widget visible (220px) with day name + number
- [ ] Event list shows compact 2-line rows (time range + title)
- [ ] Tap an event row → right column switches to detail view (partial refresh)
- [ ] Detail view shows: title, time range, location, description, calendar source
- [ ] Tap "← Back to list" → returns to event list (partial refresh)
- [ ] Tap "← Back to Week" → returns to weekly view (full refresh)
- [ ] No ghosting at the edges of the partial refresh area after multiple toggles

## Settings (debug-gated)
- [ ] Long-press battery icon (≥2s) → debug mode toggled (check serial)
- [ ] When debug mode is on, gear icon appears in footer
- [ ] Tap battery icon (debug mode) → opens settings screen
- [ ] Sidebar shows categories: Display, Power, Network
- [ ] Tap a setting row → highlights it
- [ ] Tap − or + → cycles the selected value
- [ ] Tap Save → writes to SD card and returns to calendar

## Sleep (dashboard env)
- [ ] Device sleeps after inactivity timeout
- [ ] Device wakes on button press and shows cached data
- [ ] Device wakes on timer and refreshes data
- [ ] Device sleeps immediately outside 7AM–10PM

## Battery
- [ ] Battery percentage shown in footer
- [ ] WiFi disconnects after payload fetch (check serial)
