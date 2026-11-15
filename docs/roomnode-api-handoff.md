# Room node and API contract review

This document describes the room-node implementation as of November 2026. It is
intended for review with the API team. Statements under **Implemented** describe
current firmware behavior. Statements under **Confirm with API team** are
assumptions, not settled contract.

## Product behavior

Room 1 has a wall device with three processors:

- The **main RP2040** reads the presence sensor, owns the local presence timer,
  drives the servo, and combines local and service state.
- The **ESP32-C6** owns Wi-Fi, HTTPS, device authentication, and API polling. It
  does not decide whether a room is free, held, reserved, or ending.
- The **display RP2040** renders the state supplied by main. It provides a small
  action panel, but it does not optimistically mutate room state.

The service is the source of truth for reservation state. Local presence is an
observation reported to the service, not a claim/check-in command.

## Authentication and transport

**Implemented**

- API base: `https://6526ig971l.execute-api.us-east-2.amazonaws.com`
- Room ID: `1`
- Every device request sends `x-device-key`.
- The bench network uses HTTP `CONNECT` through `192.168.49.1:8228`, followed by
  TLS to the API host. A normal deployment can disable the proxy.
- TLS validates against the ESP-IDF certificate bundle.
- HTTP error bodies are treated as plain text and logged with their status.

**Confirm with API team**

- Whether a production device key is per device, per room, or shared by a fleet.
- Rotation/provisioning requirements for production keys.

## Device endpoints

### Read current room state

```http
GET /device/rooms/1
x-device-key: <device credential>
```

The C6 polls every **5 seconds**.

Fields currently consumed:

```json
{
  "state": "free | held | reserved | warn",
  "url": "https://short.room.example/r/1?...",
  "holder": "optional display text",
  "expires_at": null,
  "next_res_start": 1794750000,
  "presence": false,
  "servo": 0,
  "alert": {
    "text": "optional display text",
    "level": 1
  }
}
```

Only `state` is required by the parser. `expires_at` and `next_res_start` may
be JSON `null`; null, missing, or nonnumeric timestamps map to zero. Missing
holder text becomes empty.

`url` is the service-owned reservation destination. It may change on any poll.
The device forwards and redraws it when it changes. If it is absent or empty,
the display says `URL unavailable` and draws no QR; there is no compiled URL
fallback. The transport accepts at most 191 URL bytes. QR Version 10 at medium
error correction fits roughly 213 byte-mode characters, but the panel renders
those larger codes at only two pixels per module. A short redirect URL is
strongly preferred over a long presigned URL.

Unknown state strings render as `UNKNOWN`.

`servo` is present on every full room response as `0` or `1`: `0` locks and
`1` unlocks. Firmware maps those values to 0% and 90% of the current simulated
servo travel. Missing or invalid values fail secure to locked. The same full
room-state parser runs on successful GET, presence, and end responses.

`alert` remains provisional. The current known device response does not provide
it, so that path is implemented but not verified against the service.

The HTTP `Date` response header supplies UTC wall time. The display converts it
to US Eastern time using EST/EDT transition rules.

### Report observed presence

```http
POST /device/rooms/1/presence
x-device-key: <device credential>
content-type: application/json

{"present":true}
```

or

```json
{"present":false}
```

**Implemented**

- Post immediately when the local presence belief changes.
- Repeat the current value every **5 seconds**.
- Reconstruct the C6's belief from main's five-second heartbeat after a C6
  reboot, then post if the service-facing value differs.
- Apply the full successful presence response immediately, including state,
  URL, timestamps, servo, alert, and server time.
- Do not infer claim, check-in, booking, or end from a presence edge.

Local estimator timing:

- Sensor output data rate: 2 Hz.
- Main reads the sensor every 200 ms.
- Entry signal: `tpresence - baseline > 1500`.
- Entry confirmation: signal remains active for 600 ms.
- Exit: no active sample for **10 seconds**.

**Confirm with API team**

- What service-side timer, if any, turns stale presence into absence.
- Whether a rejected presence POST can return statuses other than 401, 403, 404,
  or 409 and whether the device should retry faster than its normal loop.

### End the active reservation/session

```http
POST /device/rooms/1/end
x-device-key: <device credential>
content-length: 0
```

**Implemented**

- Yellow opens the display action panel.
- Green sends an end request only while state is `held` or `warn`.
- `reserved` has no active session, so the UI does not offer End.
- Display says `END REQUESTED`; it does not immediately show `free`.
- The request travels display -> main -> C6 and is executed by the C6 API task.
- A successful full end response is applied immediately; later GETs remain the
  continuing source of truth.

**Confirm with API team**

- Whether the request body must be empty, `{}`, or another JSON shape.
- Whether physical-device end requires a user identity, confirmation token, or
  audit metadata.

Confirmed: `/end` returns 200 when called in `reserved` and changes nothing;
the device avoids that no-op by not offering the action.

## Service state model

The firmware treats state as a service-owned enum. It does not compute these
transitions locally.

| Service state | Display | Local action |
| --- | --- | --- |
| `free` | Green `FREE`, reservation QR | No end action |
| `held` | Blue `IN USE`, future `expires_at` countdown | End available |
| `reserved` | Amber `RESERVED`, then `check in within` + future `next_res_start` countdown | No end action |
| `warn` | Red `ENDING`, future `expires_at` countdown, one short beep on entry | End available |
| absent/unknown/invalid | Gray `UNKNOWN` | No end action |

Transitions visible to the device are simply the result of successive GETs:

```text
UNKNOWN -> any valid service state     first successful GET
any state -> another service state     later successful GET
any state -> same state                normal five-second refresh
any state -> UNKNOWN                   not automatic; failed GET retains last state
```

Network failure sets the separate `online/offline` indicator but deliberately
retains the last valid room state. A failed poll does not invent `free`. Door
control fails secure independently: after 60 seconds without a successful GET,
the C6 commands servo 0 (locked). A later successful full response can unlock it
again with `servo: 1`.

**Confirm with API team**

- Exact semantic distinction between `held` and `reserved`.
- Whether `warn` is a real API state or should instead be derived from an expiry
  threshold on the device.
- Which timestamp applies to each state and whether it is an absolute UTC epoch
  in seconds.
- Whether `next_res_start` should be present while free and/or occupied.
- Whether `url` is a stable short redirect or a rotating capability URL, how
  long it can be, and whether the device may cache it between successful GETs.
- If `url` rotates, whether an older displayed URL remains valid for an overlap
  period long enough for a person to scan it.
- Whether holder identity is permitted on a shared wall display.
- Whether any additional states exist, such as checked-in, maintenance, locked,
  or unavailable.

## UI and controls

The UI is intentionally service-driven and shallow:

- Default screen automatically follows service state and local presence.
- It shows Eastern date/time, online/sensor health, state, presence duration,
  applicable expiry countdown, holder/alert text, and reservation QR.
- Yellow opens actions.
- Blue opens sensor details directly or from actions.
- Green confirms the available end action.
- Red or Yellow returns from a secondary screen.
- Holding Red for six seconds powers the board off; this board-level behavior
  remains active on every screen.
- Entering `warn` plays a short double-beep. If a valid expiry countdown
  reaches five seconds, it plays one more double-beep. It does not otherwise
  repeat continuously.

The diagnostics page contains sensor values and is not part of the normal room
workflow.

## USB state mock

The C6 USB console can override only the state published to main. Real API
polling, presence POSTs, and time synchronization continue underneath.

```text
mock free
mock held 120
mock reserved 300
mock warn 30
mock unknown
mock off
servo 0..100
```

`servo` is a bench command that forwards a percentage to main (1,000–2,000 µs
at 50 Hz on GPIO27). It is independent of state mocking. The optional number on
a state command is a relative expiry in seconds.

USB state mocks publish no URL, so the display deliberately shows
`URL unavailable`. `mock off` returns display state and URL to the next real GET
result. Mocked transitions never call the presence endpoint. Pressing End while
a mock is active changes the mock to `free` and never calls the real end
endpoint.

On the current C6 board, opening the USB Serial/JTAG console resets the C6. A
bench test should connect once, wait for startup, and issue all mock commands in
that session.

## Timing summary

| Operation | Interval |
| --- | ---: |
| Sensor read | 200 ms |
| Presence entry confirmation | 600 ms |
| Presence absence hold | 10 s |
| Main -> display state frame | 500 ms |
| Display repaint check | 100 ms |
| Main -> C6 heartbeat | 5 s |
| API GET poll | 5 s after the preceding request cycle |
| Wi-Fi reconnect delay | 5 s |
| Presence reminder POST | 5 s |
| Offline forced lock | 60 s since last successful GET |
| End-request acknowledgement screen | 1.5 s |
| Ending-state sound | double-beep on entry and once at 5 s remaining |

Network request time is additive: the API loop sleeps five seconds after its GET
and any required POSTs complete.

## Current verification status

Observed end to end on hardware:

- Wi-Fi association and DHCP.
- Proxy CONNECT and TLS certificate validation.
- Authenticated room GET returning `free`.
- Presence `false` POST returning HTTP 200.
- C6 -> main `status`, `net`, and `time` delivery.
- Main -> display state delivery.
- Eastern clock source propagation has compiled but still needs visual hardware
  verification.

Implemented but still requiring hardware/service verification:

- Button-driven end request through all three processors.
- API `/end` response behavior.
- USB mock command input on USB Serial/JTAG.
- `held`, `reserved`, and `warn` screen transitions.
- Ending beep volume and audibility.
- QR scanning after the larger integer scaling/two-module quiet-zone change.
- Servo `0|1` service field and offline-lock policy; API team has specified it
  but has not yet announced the field live.
- Alert fields, because the current device response does not emit them.
