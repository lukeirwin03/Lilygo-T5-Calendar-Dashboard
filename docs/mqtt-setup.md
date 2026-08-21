# MQTT Broker Setup Guide

This document describes how to configure your MQTT publisher so the calendar dashboard device receives events correctly.

## Broker connection

The device connects to the broker configured in `include/secrets.h` and `include/config.h`:

| Setting | Value | Where to change |
|---|---|---|
| **Host** | `192.168.1.38` | `config.h` → `MQTT_HOST` |
| **Port** | `1883` | `config.h` → `MQTT_PORT` |
| **Username** | (set in secrets) | `secrets.h` → `MQTT_USER` |
| **Password** | (set in secrets) | `secrets.h` → `MQTT_PASS` |
| **Client ID** | `t5-calendar-dashboard` | `config.h` → `MQTT_CLIENT_ID` |
| **Topic** | `dashboard/calendar` | `config.h` → `MQTT_TOPIC` |

If your broker is at a different address, update `MQTT_HOST` in `config.h` and rebuild.

## The one critical rule: `retain=true`

Your publisher **must** publish with the MQTT `retain` flag set to `true`:

```
mosquitto_pub -h 192.168.1.38 -t dashboard/calendar -r -f payload.json
                                                                   ^^
                                                           -r = retain
```

**Why this matters:** the device sleeps for hours at a time (deep sleep, ~0.1 mA). When it wakes, it connects to WiFi, subscribes to the topic, and expects the broker to immediately deliver the latest retained message. Without `retain=true`, the broker has nothing to deliver on subscribe, and the device would have to stay awake waiting for a new publish (burning battery).

With `retain=true`, the flow is:

```
1. Device wakes from deep sleep
2. WiFi connects (~3-10 seconds)
3. MQTT subscribes to "dashboard/calendar"
4. Broker instantly delivers the retained message
5. Device parses, renders, disconnects WiFi, goes back to sleep
```

Total awake time: ~15-30 seconds. Without retain, the device would sit idle for the full 8-second payload timeout, then give up.

## Payload format

Publish a JSON object with an `events` array. The device only reads the `events` key — any other top-level fields (like `updated` or `horizon_days`) are ignored by the device but useful for your own tracking.

### Minimal example

```json
{
  "events": [
    {
      "title": "Team standup",
      "start": "2026-08-15T09:00-05:00",
      "end": "2026-08-15T09:30-05:00",
      "all_day": false
    }
  ]
}
```

### Full example

```json
{
  "updated": "2026-08-15T12:00:00Z",
  "events": [
    {
      "title": "Summer Art Festival",
      "start": "2026-08-15",
      "end": "2026-08-17",
      "all_day": true,
      "location": "Memorial Park",
      "description": "Annual community festival",
      "calendar": "family",
      "type": "event"
    },
    {
      "title": "Client review",
      "start": "2026-08-15T14:00-05:00",
      "end": "2026-08-15T15:00-05:00",
      "all_day": false,
      "location": "Zoom",
      "description": "Quarterly review with Acme Corp",
      "calendar": "work",
      "type": "event"
    },
    {
      "title": "Gym",
      "start": "2026-08-15T18:00-05:00",
      "end": "2026-08-15T19:00-05:00",
      "all_day": false,
      "calendar": "personal"
    }
  ]
}
```

### Field reference

| Field | Required | Type | Description |
|---|---|---|---|
| `title` | Yes | string | Event name. Truncated at 63 characters. |
| `start` | Yes | string | Start date/time in ISO 8601 format (see below). |
| `end` | Yes | string | End date/time. Defaults to `start` if empty. |
| `all_day` | Yes | boolean | `true` for all-day events (date-only start/end). |
| `location` | No | string | Location text. Truncated at 47 characters. |
| `description` | No | string | Description text. Truncated at 127 characters. |
| `calendar` | No | string | Source calendar name. Drives the gray shade on the display (each unique name gets a consistent shade via hash; "main" is pinned to the light shade with black text, others cycle darker shades). Truncated at 23 characters. |
| `type` | No | string | Defaults to `"event"`. Not currently used for display differentiation. |

### Time format

Times use ISO 8601. **Important: the device ignores the timezone offset — it uses the hour and minute values as-is.** Publish event times in your local timezone so the display matches your wall clock.

**Timed events** — include the `T` separator and time:
```
"start": "2026-08-15T14:00-05:00"
"end":   "2026-08-15T15:00-05:00"
```
The `-05:00` offset is ignored by the parser. The device reads `14:00` and displays "2:00 PM" (in 12-hour mode) or "14:00" (in 24-hour mode). You can include the offset for documentation purposes, or omit it entirely:
```
"start": "2026-08-15T14:00"
```

**All-day events** — date-only, no time component:
```
"start": "2026-08-15"
"end":   "2026-08-17"
"all_day": true
```
Multi-day all-day events (start ≠ end) are automatically expanded into one entry per calendar day.

## Size limit: 4 KB

The MQTT buffer is 4096 bytes (`mqtt.setBufferSize(4096)` in the firmware). If your payload exceeds this, the message will be truncated and fail to parse. Keep payloads under 4 KB.

With typical event objects (~200 bytes each), this allows roughly **15-20 events** per payload. If you have more events than that, see the next section.

## How many days of events to publish

The device loads events within a **bidirectional window of today ± N days** into memory, where N is the device's **Context Days** setting (default 7, adjustable 1–7 on the device). The window covers today, the N days before, and the N days after. Events outside the window are saved to the SD card but not rendered. Navigation is clamped to this range.

**You can publish events of any date range** — the device will:
1. Save the full payload to `/cal/current.json` on the SD card
2. Save a deduped snapshot to `/cal/history/` if the payload changed
3. Load only events within the 12-day window into RAM for display and scheduling

Events beyond 12 days will automatically appear on the display as they enter the window (the device wakes every 2 hours to check).

**Recommendation:** publish events for **today ± N days plus a 1–2 day buffer** (e.g. with the default N=7, publish roughly `now-9d … now+9d`, ~19 days) so the device's window is always covered despite cron timing drift. The device now consumes **both past and future events** in this range — a forward-only payload will leave the "days prior" half of the display empty. Keep the total under 4 KB; if your calendar is busy, reduce N on the device or widen the publisher's horizon. The device reads the top-level `updated` field to display a data-freshness indicator and to flag staleness, so keep it accurate.

## How updates work

To add, remove, or change events, **publish a new retained message with the complete event list**. The broker replaces the old retained message. The device picks up the update on its next scheduled wake:

- **Event-aware wake** — if the changed event is sooner than 2h away and the device wakes for it
- **Periodic wake** — within 2 hours (the `Refresh Every` interval)
- **Button wake** — button wakes replay the OLD cached data (no WiFi). Fresh data appears on the next timer wake.

There is no incremental update mechanism — each publish replaces the entire event list.

### Clearing all events

Publish an empty events array with retain:
```bash
mosquitto_pub -h 192.168.1.38 -t dashboard/calendar -r -m '{"events":[]}'
```

## Testing with mosquitto

Install `mosquitto-clients` on your computer:

```bash
# Ubuntu/Debian
sudo apt install mosquitto-clients

# macOS
brew install mosquitto
```

### Publish a test payload

```bash
mosquitto_pub -h 192.168.1.38 -t dashboard/calendar -r -f example-payload.json
```

### Verify the retained message

```bash
mosquitto_sub -h 192.168.1.38 -t dashboard/calendar -C 1
```
(The `-C 1` flag exits after receiving one message. If you just subscribed without `-C`, you'd see the retained message immediately, then wait for new publishes.)

### Watch for new publishes in real time

```bash
mosquitto_sub -h 192.168.1.38 -t dashboard/calendar -v
```

### Quick inline test

```bash
mosquitto_pub -h 192.168.1.38 -t dashboard/calendar -r -m '{
  "events": [
    {
      "title": "Test event",
      "start": "2026-08-15T12:00",
      "end": "2026-08-15T13:00",
      "all_day": false,
      "calendar": "test"
    }
  ]
}'
```

## Sample publisher script (Python)

Here's a minimal Python script that demonstrates the publish pattern. Adapt it to your calendar source (Google Calendar API, CalDAV, ICS feed, etc.):

```python
#!/usr/bin/env python3
"""Publish calendar events to the dashboard MQTT topic."""

import json
import paho.mqtt.client as mqtt
from datetime import datetime, timedelta

BROKER = "192.168.1.38"
PORT = 1883
TOPIC = "dashboard/calendar"
USERNAME = "your_mqtt_user"      # set in secrets.h
PASSWORD = "your_mqtt_password"   # set in secrets.h
DAYS_AHEAD = 14                   # publish 14 days of events

def build_payload(events):
    """Build the JSON payload from a list of event dicts."""
    return json.dumps({
        "updated": datetime.utcnow().isoformat() + "Z",
        "events": events,
    })

def publish(events):
    """Publish events to MQTT with retain=true."""
    payload = build_payload(events)
    if len(payload) > 4096:
        print(f"WARNING: payload is {len(payload)} bytes (max 4096)")

    client = mqtt.Client()
    client.username_pw_set(USERNAME, PASSWORD)
    client.connect(BROKER, PORT)
    client.publish(TOPIC, payload, retain=True)
    client.disconnect()
    print(f"Published {len(events)} events ({len(payload)} bytes)")

# Example usage:
if __name__ == "__main__":
    # Replace this with your calendar source (Google API, CalDAV, etc.)
    events = [
        {
            "title": "Standup",
            "start": "2026-08-15T09:00",
            "end": "2026-08-15T09:30",
            "all_day": False,
            "calendar": "work",
        },
        {
            "title": "Weekend trip",
            "start": "2026-08-16",
            "end": "2026-08-17",
            "all_day": True,
            "calendar": "personal",
        },
    ]
    publish(events)
```

Run it on a cron schedule (e.g., every 30 minutes) so the retained message stays current:

```bash
# crontab -e
*/30 * * * * /usr/bin/python3 /path/to/publish_calendar.py
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Device shows "Connecting..." for a long time on cold boot | Broker unreachable or credentials wrong | Check `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS` in config/secrets |
| Device connects but shows no events | `retain=true` not set on the publish | Re-publish with `-r` flag |
| Events appear but times are wrong | Times published in UTC instead of local time | Convert to local time before publishing (offset is ignored by device) |
| Some events missing from display | Payload over 4 KB (truncated) | Reduce the number of days or events per publish |
| Events beyond 12 days don't appear | Working as designed — 12-day window | Events appear automatically as they enter the window |
| Device doesn't pick up new events quickly | Timer wake interval is 2h | Reduce `Refresh Every` in settings, or wait for the next event-aware wake |
| Button wake shows stale data | Button wakes don't connect WiFi (by design) | Fresh data appears on the next scheduled timer wake |
