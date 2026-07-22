# UI Redesign Plan — Weekly View

> Living document — update as decisions change.
> Scope: weekly view redesign (complete). Daily view and future work tracked below.
> Last updated: July 22, 2026

## Goals

1. **Duration legibility** — visual and textual signals for how long events are.
2. **Better day-of-week headings** — bigger, more readable, with hierarchy.
3. **Data-reactive rendering** — layout adapts to what's actually on the schedule.
4. **More text per event** — multi-line, real measurement, word wrapping.
5. **New font system** — clearer typography for body / headings / times.

## Font system

| Role | Font | Sizes in use | Status |
|---|---|---|---|
| Day-of-week headings | **Genty24** | 24 pt | ✅ Production |
| Event body text (time, title) | **MeltSwashes18** | 18 pt | ✅ Production |
| Event titles (short events) | **MeltSwashes16** | 16 pt | ✅ Production |
| Location line, indicators | **MeltSwashes14** | 14 pt | ✅ Production |
| Daily view event titles | **Genty24** | 24 pt | ✅ Production |
| Settings UI labels | **MeltSwashes14/16** | 14, 16 pt | ✅ Production |
| Large display (daily tear-off) | **Genty48** | 48 pt | ✅ Production |
| Computer (pixel font) | **Computer16** | 16 pt | ✅ Settings UI only |

### Evaluated and rejected (July 2026)
- **CapitolCity** — clean sans, evaluated for body text. Rejected: Genty/MeltSwashes looked better on hardware.
- **Platinum Sign Over/Under** — decorative headings with shadow. Rejected: too ornate for the panel's 1-bit rasterization.
- **DS-DIGI** — 7-segment for times. Rejected: too thin at small sizes, segments broke below 24 pt.
- **Genty16** — too small for the bumped typography. Replaced by Genty24.

### Constraints
- **GFXfont rasterization is 1-bit** (no anti-aliasing). Clean fonts hold up better at small sizes.
- **No font fallback chain** — missing glyphs render as blank. All converted fonts cover ASCII 32-126.
- **White text on dark fills** gets a 1px black outline (8-direction halo) for readability.

## Phase roadmap

### Phase 0 — Foundation ✅ COMPLETE
- [x] Fix `convert_fonts.sh` (case-sensitive bug, atomic writes, freetype check, pragma wrappers)
- [x] Generate all font headers via the fixed script
- [x] Build font swatch screen (evaluated fonts on hardware, then removed swatch)
- [x] Decide which sizes to use: Genty24 headings, MeltSwashes14/16/18 body (CapitolCity/Platinum/DSDigi rejected)

### Phase 1 — Text measurement + layout cleanup
- [x] `measureText()` helper wrapping `get_text_bounds`
- [x] Replace all hardcoded `chars * 12px` estimates with real measurement
- [x] Drop the top window border (regain ~24px vertical space)
- [x] Today-column highlight (subtle background tint or bolder header)

### Phase 2 — Text density (the biggest perceived win)
- [x] Word-wrap titles to N lines based on `blockH`
- [x] Word-boundary truncation (no mid-word ellipsis)
- [x] Add location as line 2 when block is tall enough
- [ ] Try description text on very tall blocks (D5 — might back out)

### Phase 3 — Duration legibility (PARTIALLY COMPLETE)
- [x] Time display: start time at top, end time at bottom (see Phase 7 for final implementation)
- [x] 3-hour grid lines (restored after hour-band experiment was reverted)
- [x] Proportional block heights for short events (see Phase 5)
- [~] ~~Alternating hour bands~~ — REVERTED (user preferred original grid lines)
- [~] ~~Scaled minimum block height~~ — superseded by Phase 5 proportional heights

### Phase 4 — Empty-day placeholder
- [x] Empty-day column shows "No events" centered instead of empty timeline

### Phase 5 — Short event compression
- [x] New rendering path for events ≤ 60 min: one-line "Time Title..." format
- [x] Format: `"3:30PM Chiro..."` — time first (no space before AM/PM for compactness), then title, truncated to fit
- [x] Proportional block height for events in the 30 min – 3 hr range
- [x] Min height (~32 px so the single line fits) and max height (events 3 hr+ render at a consistent size)
- [~] Duration indicator bar — DEFERRED: block height itself signals duration proportionally; revisit if hardware testing shows it's insufficient

**Note (Phase 10)**: The 1-line short-event format is most relevant in the narrower context columns. The focus column has 2× width so 2-line layout fits comfortably even for short events. We may revert to 2-line for the focus column in Phase 10b.

### Phase 6 — Empty-time-gap indicator
- [x] Draw a "nothing happening" symbol in gaps > 1 hour between events
- [x] Symbol: dotted horizontal line `· · · · ·` (tried and landed on this as the cleanest option)
- [x] Subtle gray (LTGRAY), not aggressive

### Phase 7 — Time display refinement
- [x] Show start time at top of block (revert the range text)
- [x] Show end time at bottom of block (inside the block, near the bottom edge)
- [x] Suppress end-time display when an overlapping event follows (the next event's block would cover that area anyway)
- [x] Add overlap-detection helper

### Phase 8 — Overlap handling
- [x] **Approach A — Lane splitting**: implemented in focus column (greedy interval coloring, 4 px lane gaps)
- [~] **Approach B — Combined block**: DEFERRED — lane splitting works well; revisit only if hardware testing reveals issues

### Phase 9 — Header polish
- [x] Today-column highlight (subtle LTGRAY tint behind header)
- [~] Two-line day header — DEFERRED: current single-line Genty24 works well
- [~] Reactive font sizing — N/A: hard-coded to 3 columns (focus+context layout)

### Phase 10 — Focus + context view (replaces the equal-column weekly layout)
The weekly view is now 3 columns: 1 focus (center, 2× width) + 2 context (left/right, 1× width each). Default focus is today. Navigation is arrow-based: ±1 day per tap, "Today" button jumps back, context-column taps move focus to that day, and focus-column tap opens the daily view.

- [x] **10a — Layout + state + navigation** ✅
  - 3 columns: context (left), focus (center, 2× width), context (right)
  - Arrows move focus by 1 day per tap
  - Tap context columns = navigate (±1 day); tap focus = daily view
  - "Today" button in left footer = jump to today
  - Full day names in focus header; short names in context
  - Today highlight per-column
  - Swipe and long-press navigation REMOVED (simplified to arrows + Today button)
- [x] **10b — Focus column verbose rendering + lane splitting**
  - Focus column shows the same content but benefits from ~2× horizontal room
  - Implement lane splitting for overlapping events (side-by-side blocks)
  - End-time rendering can be more aggressive (more room available)
- [x] **10c — Context column polish**
  - Verified context columns render readably at ~224 px width
  - Tuned: relaxed end-time suppression in the focus column when lane splitting is active (overlapping followers are now side-by-side, so they no longer cover the end-time area)
- [x] Remove `num_columns` setting (no longer configurable — always 3)

## Truncation policy

When truncating text to fit a width:
- Prefer word boundaries (don't cut mid-word if a word boundary nearby also fits)
- BUT maximize the text shown — don't leave room unused if more text could fit
- Last line of a wrapped block should use `truncateToFitWidth` on the FULL remaining text (not just append "..." to whatever was already on that line)
- The result should always be the longest possible prefix (at a word boundary if possible) that fits with "..." appended

## Decisions log

- **2026-07-19** — Confirmed direction on all four goal areas:
  - Duration: A1 (time range) + A2 (hour bands) + A5 (scaled min height). Skip A3 (patterns), A4 (drop min entirely).
  - Dynamic: C1 (per-column range) + C2 (empty placeholder) + C3 (height-based line budget) + C5 (drop top border). Defer C4 (lanes).
  - Text density: D1 (measure) + D2 (wrap) + D3 (location) + D4 (word-boundary truncation). Try D5 (description) experimentally.
  - Header polish: B1 (two-line) + B2 (new fonts) + B3 (today highlight) + B4 (reactive sizing).
- **2026-07-19** — Font roles assigned: CapitolCity body, Platinum Over/Under headings, DS-DIGI times.
- **2026-07-19** — convert_fonts.sh had a fatal case-sensitivity bug (`DS-DIGI.ttf` vs actual `DS-DIGI.TTF`). Fixed in same pass.
- **2026-07-20** — User feedback after on-hardware eval of Phases 1-3:
  - Hour bands REVERTED — user preferred the original 3-hour LTGRAY grid lines.
  - Time range text at top REVERTED — user prefers start time at top, end time at bottom (with overlap-aware suppression). See Phase 7.
  - Short events should use one-line format `"3:30PM Chiro..."` (time first, no space before AM/PM).
  - Proportional heights: 30 min – 3 hr scales linearly; ≥ 3 hr uses a consistent size.
  - Overlap handling: try BOTH lane splitting and combined blocks; pick the better on hardware.
  - Empty time gaps: try a symbol (∴ or dotted line); iterate on hardware.
  - Truncation: keep word-boundary preference but make it less aggressive — fit more text.
- **2026-07-21** — Phase 10 designed after user requested focus+context layout:
  - 3 columns: 1 focussed (2× width) + 2 context (1× width each).
  - Context columns show full timeline with truncated event blocks (option d).
  - Focus column shows verbose content + lane splitting for overlaps.
  - Tap arrows = ±1 day, swipe = ±3 days, long-press left arrow = jump to today.
  - Tap any column opens daily view (focus shift is via arrows/swipe only).
  - Full day names in headers.
  - Today highlight per-column (applies to whichever column shows today).
  - `num_columns` setting removed (always 3).
- **2026-07-21** — All-day banner moved from a separate ruled section above the timeline to an overlaid black rectangle at the very top of the timeline area (36 px tall). The timeline regains the former banner reservation, and events do not shift down — grid alignment is preserved. Added out-of-range event indicators (`↑ N before XAM` / `↓ N after XPM`) at the top/bottom of the timeline when events fall outside the visible range.
- **2026-07-22** — UI redesign committed (commit `cc98492`). All phases 1-10 complete or deferred-with-rationale. 35 files changed. Touch diagnostics added to demo env. 26 test events covering edge cases. Focus+context layout live with lane splitting, proportional heights, all-day banner relocation, gap indicators, and out-of-range indicators.

## Key files

| File | Role |
|---|---|
| `src/ui.cpp` | All weekly + daily view rendering, touch state machine, gesture classification |
| `src/main.cpp` | Demo entry point, 26 test events covering edge cases, touch diagnostics |
| `src/main_dashboard.cpp` | Production entry point, MQTT pipeline, sleep/wake cycle |
| `src/ui_settings.cpp` | Settings screen (debug-gated), sidebar + row navigation |
| `src/settings.cpp` | Runtime settings (load/save from SD card, overrides config.h defaults) |
| `src/sd_storage.cpp` | SD card: event caching, settings file, log files |
| `src/display_manager.cpp` | PSRAM framebuffer, full/partial refresh, splash screen |
| `src/power_mgr.cpp` | Deep sleep + wake config, light sleep (scaffolded, unused) |
| `convert_fonts.sh` | Font conversion pipeline (Genty + MeltSwashes, atomic writes, pragma wrappers) |
| `UI_REDESIGN.md` | This file — living design document |

## Success criteria

Met if these are observable on hardware:

- [ ] Focus+context layout: focus column is visibly wider and extends to the bottom of the screen
- [ ] A 30-minute event is visually shorter than a 60-minute event (proportional heights)
- [ ] Day-of-week header is readable at arm's length (Genty24)
- [ ] Overlapping events render side-by-side in the focus column (lane splitting)
- [ ] An empty day shows "No events" centered
- [ ] Today's column gets a subtle highlight
- [ ] Back-to-back events have a visible gap between them
- [ ] Multi-day all-day events show continuation arrows (← →)
- [ ] Events before 7 AM or after 10 PM trigger count indicators
- [ ] Tap arrows navigates ±1 day; tap "Today" jumps back; tap focus opens daily view

## Testing Checklist

After flashing (`pio run -e demo -t upload`), verify each feature on hardware.

### Navigation (Phase 10a)
- [ ] Default focus is today (center column, 2× width)
- [ ] Tap left arrow → focus moves back 1 day
- [ ] Tap right arrow → focus moves forward 1 day
- [ ] Tap left context column → focus moves back 1 day
- [ ] Tap right context column → focus moves forward 1 day
- [ ] Tap focus column → opens daily view for that day
- [ ] Tap "Today" button → focus jumps back to today
- [ ] Full day name in focus header ("Thursday"); short name in context ("Thu")

### Typography (Phases 5, 7)
- [ ] Short events (≤60 min) show 1-line format: "3:30PM Chiro..."
- [ ] Longer events show 2-line format: time on top, title below
- [ ] End time appears at bottom of tall blocks (when no overlap follows)
- [ ] End time is suppressed when overlapping follower exists
- [ ] Proportional block heights: 30 min visibly shorter than 60 min

### Event rendering (Phases 2, 5, 10b)
- [ ] Long titles word-wrap to 2 lines on tall blocks
- [ ] Location appears on very tall blocks (below title)
- [ ] Truncation uses "..." at word boundaries (not mid-word)
- [ ] White text on dark fills has a black outline (readable)
- [ ] Lane splitting: overlapping events in focus column render side-by-side
- [ ] Context columns stack overlapping events (no lane splitting)
- [ ] Visual gap between adjacent events (4 px)

### All-day banner (Phase 10 + relocation)
- [ ] Banner appears at top of timeline (not separate section)
- [ ] Multi-day events show arrows: "← Title" / "Title →" / "← Title →"
- [ ] Days with no all-day events have no banner (timeline starts clean)
- [ ] Banner doesn't overflow into the header above

### Time range and indicators (Phases 3, 6)
- [ ] 3-hour grid lines align horizontally across all 3 columns
- [ ] Dotted gap indicators appear between events with >1 hour gap
- [ ] "↑ N before 7AM" indicator appears when events exist before 7 AM
- [ ] "↓ N after 10PM" indicator appears when events exist after 10 PM
- [ ] "No events" placeholder appears on empty days

### Layout (Phase 10 overhaul)
- [ ] Focus column extends to bottom of screen (past context columns)
- [ ] Left footer: back arrow + "Today" button
- [ ] Right footer: battery + forward arrow
- [ ] No date title in footer (each column shows its own date)
- [ ] Today column gets a subtle background tint (when visible)

### Edge cases (expanded test data)
- [ ] Day 0: pre-7AM indicator shows "↑ 1 before 7AM"
- [ ] Day 1: triple overlap renders as 3 side-by-side lanes in focus column
- [ ] Day 1: post-10PM indicator shows "↓ 1 after 10PM"
- [ ] Day 2: 15-min event renders as very short block
- [ ] Day 2: 4-hour event renders with full detail (title + location + end time)
- [ ] Day 5: "No events" placeholder centered in column
- [ ] Day 6: two all-day banners on same day (multi-all-day)

## Next steps — prioritized

### Immediate (verify before more work)
1. **Hardware verification** — flash `pio run -e demo -t upload` and run through the Testing Checklist below. This is the gating step for all further work.
2. **Dashboard env touch verification** — the touch diagnostics were only added to `main.cpp` (demo). Flash `pio run -e dashboard -t upload` and verify touch works with real MQTT data.

### Short-term (after hardware verification passes)
3. **Daily view typography update** — the daily view still uses the old Genty20/MeltSwashes14 fonts. Bump to match the weekly view (Genty24/MeltSwashes18). Low risk, high visual consistency win.
4. **README.md rewrite** — currently marked stale. Rewrite to describe the focus+context layout, current navigation model, and updated feature set.
5. **Phase 9: Two-line day header** — if the single-line header feels cramped on hardware, implement day name (Genty24) above date (MeltSwashes14).

### Medium-term (feature additions)
6. **Power optimization wiring** — `power_mgr::lightSleep()` and `enableTouchWake()` are scaffolded but unused. Wire them into the main loop to drop idle power from ~80 mA to ~0.8 mA. Significant battery life improvement.
7. **Phase 8 Approach B** — combined-block experiment (if lane splitting has issues on hardware with 3+ overlapping events).
8. **Description text on tall blocks** — Phase 2 D5 experiment. Show event description on very tall blocks (>150 px).

### Long-term (new features)
9. **Month view** — calendar grid for browsing historical/future months.
10. **History browsing** — load past events from SD card (`sd_storage::loadEventsForDate` is already implemented but unused in rendering).
11. **Settings UI typography** — update settings screen fonts to match the weekly view.

### Decided against (won't do)
- Pattern fills by duration (A3) — abandoned after hour-band experiment
- DS-DIGI / CapitolCity / Platinum fonts — rejected on hardware
- Swipe navigation — removed in favor of simpler arrow-only model
- `num_columns` setting — hard-coded to 3 for focus+context
