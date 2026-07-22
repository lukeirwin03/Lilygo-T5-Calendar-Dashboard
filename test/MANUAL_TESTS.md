# Manual Hardware Tests

Run these on the actual device after flashing with `pio run -e dashboard -t upload`.

## Boot
- [ ] Device shows "Connecting..." splash on cold boot
- [ ] WiFi connects (check serial: `[wifi] Connected`)
- [ ] MQTT connects and receives payload (check serial: `[mqtt] Connected successfully!`)
- [ ] Calendar renders with events (or empty columns if no events)
- [ ] SD card mounts (check serial: `[sd] SDHC card, XXXXX MB`)

## Touch Navigation
- [ ] Tap right arrow → advances to next day/week
- [ ] Tap left arrow → goes to previous day/week
- [ ] Tap a weekly column → opens daily view for that day
- [ ] Tap "Back to Week" button → returns to weekly view
- [ ] Swipe left/right in header → navigates
- [ ] Long press (>500ms) → ignored (no action)

## Sleep
- [ ] Device sleeps after 45 seconds of no interaction
- [ ] Device wakes on button press and shows cached data
- [ ] Device wakes on timer (every 2 hours) and refreshes data

## Settings Screen
- [ ] Tap gear icon in header → enters settings screen
- [ ] Sidebar shows 3 categories: Display, Power, Network
- [ ] Tap a category → switches content area
- [ ] Tap a setting row → highlights it (gray bar)
- [ ] Tap − or + at bottom → cycles the selected value
- [ ] Selecting a different row → old row clears to white, new row highlights
- [ ] Tap Save → writes to SD card and returns to calendar
- [ ] Tap Back → returns to calendar without saving

## SD Card
- [ ] Events saved to `/cal/YYYY-MM-DD.json` after MQTT payload
- [ ] Logs written to `/logs/YYYY-MM-DD.log`
- [ ] Old logs (>3 days) deleted on boot
- [ ] Settings persisted in `/config/settings.json`

## Battery
- [ ] Battery percentage shown in header
- [ ] WiFi disconnects after payload fetch (check serial: `[wifi] Disconnected`)
