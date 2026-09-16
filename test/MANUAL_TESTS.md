# Manual Hardware Tests

Run these on the actual device after flashing.

## Boot & connection screen (dashboard env)
- [ ] With a valid SD cache + RTC clock: calendar renders immediately from cache (no connection screen), then re-renders if fresh data arrives
- [ ] First boot / no cache / after power loss: connection screen shows — "Calendar" wordmark with flanking rules, empty progress bar, "Starting up"
- [ ] Stage label advances: "Connecting to WiFi" → "Syncing clock" → "Connecting to broker" → "Fetching calendar"
- [ ] Progress bar fills as stages complete; while waiting it creeps forward slowly and the label dots animate (partial refreshes, no full-panel flash)
- [ ] Failure labels show when a stage fails (e.g. "WiFi unavailable" with AP off) and boot still falls back to cached data
- [ ] WiFi connects (check serial: `[wifi] Connected`)
- [ ] MQTT connects and receives payload (check serial: `[mqtt] Connected successfully!`)
- [ ] SD card mounts (check serial)

## Weekly view — focus+context layout
- [ ] Three columns visible: left context (text list), focus (event blocks), right context (text list)
- [ ] Focus column has start/end time labels on boundary lines
- [ ] Focus column has two grid lines at 1/3 and 2/3
- [ ] Event blocks are proportional to duration; short events stay readable (min-height ramp)
- [ ] Back-to-back events fill their time slots with only a small deliberate gap between them
- [ ] Overlapping events are lane-split (side by side)
- [ ] Gaps between events show faint gray fill
- [ ] All-day events show in a black banner above the timeline; multi-day ones get ← / → arrows
- [ ] Empty days show "No events" placeholder
- [ ] Timeline duration never snaps below 6h
- [ ] Today's column has a light-gray header band
- [ ] At the ±Context-Days edge, the adjacent context column is hidden

## Weekly view — sliding window (today; easiest to verify on the demo build)
- [ ] With one event 2–3 PM: morning render shows a window from Day Start (~7–8 AM) to ~4 PM
- [ ] After each wake/refresh the window slides forward — start tracks roughly now − 1h (rounded to :00/:30)
- [ ] Past events scroll up off the top of the window; upcoming events are never hidden by the slide
- [ ] After the last event ends the window freezes with the last event pinned at its top; the now marker keeps moving down on later renders
- [ ] Now marker: inward-pointing chevron pair ('> · <') at the column edges while "now" is inside the window
- [ ] Now marker clamps just inside the top boundary with a ▲ chevron before the window starts (early morning)
- [ ] Now marker clamps just inside the bottom boundary with a ▼ chevron after the frozen end (evening)
- [ ] A small ▲ arrow appears at the top-right of the timeline only when past events are clipped off
- [ ] Tapping ▲ pages back (window-length step) and the boundary labels update; ▼ appears to return
- [ ] Tapping ▼ returns toward the live window; arrow disappears once nothing is clipped
- [ ] Scroll renders use the fast ghost refresh — prior timeline frames may leave faint ghosts; a later full refresh (navigation, new data) cleans them up
- [ ] Scroll offset resets after the next data refresh or day navigation
- [ ] Navigating to non-today days shows the deterministic compact event window (no arrows, no now marker)

## Demo build — differential-refresh calibration screen
- [ ] Runs at boot (before the connection animation); loops cycles until the button is pressed
- [ ] Every cycle starts with a clean reset of all cells (×1 masked erase + fresh base ink) — no text stacking at the base-word re-ink on any row
- [ ] ERASE x1 / DIFF SWAP rows: quality references (single-pass erase; shipping full-swap timing)
- [ ] GAP 0 / GAP 100 / GAP 250 rows: same swap with shrinking settle gaps — compare cleanliness against DIFF SWAP; serial reports each phase's measured duration
- [ ] RAPID x2 row: six gap-0, 2-pass-ink swaps back to back (total + average time reported) — the shipping configuration (gap 0, ink ×2 validated); watch for charge artifacts over repeated cycles
- [ ] Serial log narrates and times every step (`[difftest]`, `[diff]`)
- [ ] Connection screen: stage text erases fully with no offset ghosting (the 1-bit ink frames span the full panel height — sub-height frames land a row off from the 4-bit erase path)

## Demo build — photo walk-through
- [ ] After the calibration screen, boot plays the connection-screen animation for two cycles (all-success, then one ending in the "No payload received" failure label); band updates are flash-free (differential refresh) and old stage text should erase to clean white; touch or button skips
- [ ] If the white-ink erase leaves faint residue or the erase/draw still smear together, tune in display_manager.cpp's diffRefresh: `ERASE_PASSES` (erase strength, try 4-5), `ERASE_SETTLE_MS` (settle time between erase and draw), `INK_PASSES`/`INK_TIME_US` (solidity of the new ink), and `INK_MAX` (what counts as ink); the cleared partialRefresh fallback is a one-line swap in conn_screen's drawBand
- [ ] Simulated clock starts at 9:42 AM (serial: `[demo] Simulated clock: ...`)
- [ ] At 9:42 the today column shows the sliding window with the now marker near the top and the ▲ scroll arrow (the 6:30 AM jog has scrolled off)
- [ ] Long-press the button (≥0.8 s) → clock jumps +2h and re-renders; short press still toggles the settings modal
- [ ] Repeated jumps walk through: mid-day slide → last event done → frozen window with the last event at top → marker pinned ▼ at the bottom
- [ ] Past midnight the clock wraps to 6 AM of the same demo day
- [ ] Tap ▲/▼ scroll arrows and observe the ghost-refresh experiment — expected on this driver: old event blocks are NOT erased (a white target drives nothing), so blocks appear duplicated at their old and new positions until the next full refresh; judge whether that trade is acceptable or whether scroll should use the cleared refresh instead

## Weekly view — context columns
- [ ] Left context shows previous day's events as text list (time + title)
- [ ] Right context shows next day's events as text list
- [ ] Time format: "9:00-10:00AM" (same half) or "11:30A-1:00P" (crossing AM/PM)
- [ ] Alternating row stripes (white/LTGRAY)
- [ ] Long event lists show "+N more..." at the bottom

## Navigation
- [ ] Tap left context column → goes back 1 day
- [ ] Tap right context column → advances 1 day
- [ ] Tap focus column → opens daily view
- [ ] Navigation clamps at the ±Context-Days boundary (no further movement)
- [ ] No duplicate navigation (touch release gate works)
- [ ] Tapping during render is consumed (no stale tap after refresh)

## Daily view
- [ ] Status strip shows event counts ("Events: N | All-day: M") and, on today with clock set, "Now H:MM"
- [ ] Tear-off calendar widget visible (220px) with day name + number
- [ ] Event list shows compact 2-line rows (time range + title)
- [ ] Prev/next arrows navigate days; they render gray (disabled) at the window edge
- [ ] Tap an event row → right column switches to detail view (partial refresh)
- [ ] Detail view shows: title, time range, location, description, calendar source
- [ ] Tap "← Back to list" → returns to event list (partial refresh)
- [ ] Tap "Return" (nav container) → returns to weekly view (full refresh)
- [ ] No ghosting at the edges of the partial refresh area after multiple toggles

## Settings modal
- [ ] Button press → settings modal opens over the current view (flash-free differential refresh)
- [ ] Title bar shows battery glyph + percentage
- [ ] Tabs: Display, Power, Diagnostics
- [ ] Tap a setting row → highlights it
- [ ] Tap − or + → cycles the selected value (row updates via partial refresh)
- [ ] Diagnostics tab shows Last Updated age, WiFi/MQTT last-attempt outcome ("Last: OK" / "Last: fail" after a refresh; "Off" before the first attempt after power-on; live "On −xxdBm" only if opened mid-connection), battery, free memory
- [ ] Tap Save → writes `/config/settings.json` to SD card and closes the modal
- [ ] Tap Sync → closes the modal and forces a fresh MQTT pull (calendar re-renders when data lands)
- [ ] Tap Close (or button) → returns to the previous view (brief cleared reflash of the modal's rows — no full-screen flash)
- [ ] Changing Context Days applies on modal close (event window reloads from cache)

## Settings modal flash-free (diff refresh)
- [ ] Open the modal with the button → ONE gentle white wipe touching ONLY the modal's columns (x 100..860 of rows 60..480), then the modal inks in — no flashing clear cycle anywhere
- [ ] The strips of underlying view beside the modal (x < 100 and x ≥ 860) keep their ink through open and every in-modal update — no white flash band on open, no churn there on row taps
- [ ] The white wipe on open also erases any ghost-scroll residue left under the modal's columns by earlier timeline scrolls
- [ ] Tap rows / tap − or + → fast binary swap of just the affected row band, driving only the modal's columns (~half a second, no flash)
- [ ] Switch tabs → tab content swaps; the strips, title bar and bottom bar are passed as persistent rects and stay untouched (their ink is never re-driven — watch for zero flicker there)
- [ ] Close (button, Close X, or Save) → a brief cleared reflash of the modal's row range (rows 60..480): a localized flash there is expected and intentional — it resets the pixels and pulls fresh data; the strips reflash with it because the clear is full-width
- [ ] Restyle check: selected rows render INVERTED (black fill, white label/value/">" glyph); active tab, ± triangles, Save/Sync buttons and the separator line are solid black — no grays anywhere in the modal (grays threshold to black under the 1-bit ink path)

## Cache & dedup
- [ ] Publish a payload including past events that are also in the day cache → each occurrence renders exactly once (no duplicates)
- [ ] With WiFi/broker down, cold boot still shows the last cached calendar
- [ ] `/cal/current.json`, `/cal/history/*.jsonl`, `/cal/cache/*.json`, `/logs/*.log` all appear on the SD card

## Sleep (dashboard env)
- [ ] Device sleeps after the Sleep After inactivity timeout (default 1 min)
- [ ] Every refresh keeps the device awake and touch-responsive for at least 15 s regardless of input
- [ ] Any touch/button input resets the inactivity timer
- [ ] Before sleeping, display resets to the weekly view centered on today
- [ ] Button wake → cached data shows instantly, then a background refresh re-renders if fresh data lands (~10–30 s)
- [ ] Timer wake → data refreshes and re-renders
- [ ] Timer wakes inside the sleep window (default 10 PM–7 AM) skip WiFi and return to sleep immediately
- [ ] Sleep duration lands just past the Refresh Every boundary (e.g. :00:10 for 1h)

## Power / misc
- [ ] Battery percentage in the settings title bar matches a voltmeter reading
- [ ] WiFi disconnects after payload fetch (check serial)
- [ ] Serial heartbeat (`---- health check ----`) every 30 s while awake
