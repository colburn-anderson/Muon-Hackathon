# Room presence / reservation node — architecture

One fixed room, `room_id = 1`. The FreeWili OG is a **simple instruction
follower**: it senses the room, reports it, and obeys what the backend tells it.
All policy lives in the AWS API; the device is not allowed to invent state.

Status: implemented and running on hardware. This document is the wire-level
contract — who owns what, what travels on which wire, and the exact shapes of the
payloads. The API-facing contract, timings and per-feature verification status
live in `roomnode-api-handoff.md`; where the two disagree, that document is the
newer one.

For the reasoning rather than the wiring — the sensor filtering chain, why the
debounce is asymmetric, what happens on each kind of failure, and the division of
labour between the three CPUs — see `docs/roomnode-architecture.md`.

## Hardware map

| Thing | Where | Notes |
| --- | --- | --- |
| STHS34PF80 presence sensor | **main CPU breakout I2C0, SDA GPIO16 / SCL GPIO17** | replaces the WiFi-CSI experiment entirely |
| Door-lock servo | **main CPU breakout GPIO27** | PWM, open-loop; not attached yet |
| ESP32-C6 | on the Bottlenose Orca | Wi-Fi + HTTPS to the API |
| ESP32 ↔ main link | main CPU **UART1, GPIO8 TX / 9 RX / 10 CTS / 11 RTS**, 115200 baud 8N1, RTS/CTS | low-rate JSON control link |
| main ↔ display link | **UART0** both CPUs, `fwog_link` framing in `bsp/common` | UART0 must never carry stdio |
| LCD, buttons, WS2812 | display CPU | UI, yes; the display CPU owns no policy |
| MCP7940 RTC | display CPU I2C1 @ 0x6F | optional wall clock source |

The C6 sits *behind* the main CPU: it has no sensor and no servo. That keeps
the network firmware dumb (bytes in, bytes out) and the timing-sensitive work
on the RP2040s, where a custom firmware is not going to reflash itself mid-demo.

## Who owns what

**Main CPU — the state machine.**
- Reads the sensor, debounces it into `present` / `absent` with a hold-off
  (a person sitting still must not read as absent; a person walking past must
  not read as present — the sensor's own presence/temperature outputs plus a
  time filter).
- Owns `presence_since` (monotonic) and therefore the elapsed time.
- Drives the servo to the angle the backend asked for.
- Relays display-bound state over UART0.
- Survives the network being down: no IP, no API, still senses and displays.

**ESP32-C6 — the network side.**
- Wi-Fi station; optionally through the HTTP proxy (`192.168.49.1:8228`),
  compiled in but off by default.
- Publishes presence transitions and heartbeats to the API.
- Polls for the two commanded things: servo target and alert banner.
- No decision making: a failed poll means "keep the last known command", not
  "improvise".

**Display CPU — pixels only.**
- Room state (`free|held|reserved|warn`), elapsed time when occupied, the alert
  banner and its reason, and a QR code.
- QR encodes the service-owned `url` field from the latest successful room GET.
  A missing or empty field displays `URL unavailable`; firmware has no compiled
  fallback destination.
- Buttons are for bring-up (screen cycling); the reservation flow lives in the
  web app the QR points at.

## State model

`state` arrives from the API; the device never computes it.

| State | Display | Servo |
| --- | --- | --- |
| `free` | green `FREE` + QR to reserve | locked |
| `held` | blue `IN USE` + countdown to `expires_at` | locked |
| `reserved` | amber `RESERVED`, then `check in within` + countdown to `next_res_start` | locked |
| `warn` | red `ENDING` + countdown, double-beep on entry and at 5 s left | locked/unlocked per command |
| absent/unknown | gray `CHECKING` (link up) or `CONNECTING` (link down) | locked |

While occupied the left column additionally shows elapsed `mm:ss` since
presence began.

Three things on the panel are about honesty rather than state, because a status
display is only useful if a person can tell when it is guessing:

- With the link down, the full-width band says `No connection / This may be out
  of date` in amber. It outranks the derived `Ending soon` strip, since that
  sentence is reconstructible from the state word and countdown, while nothing
  else on screen admits the panel may be stale. A service-written alert still
  outranks both.
- With the sensor not answering, the presence row reads `presence unknown`
  instead of holding its last verdict, matching what main actually claims (it
  holds the belief, it does not assert an empty room). `net up` also expires:
  fifteen seconds of silence from the C6 turns it into `down`, so the band
  cannot be defeated by a dead wire.
- The bottom hint says what the buttons can do *here* -- `scan code to reserve`
  when free, `Y actions to release` when held or ending, `B details` when
  offline (End travels to the service through the C6, so with no link it is not
  a button). Elapsed-on-display is local and monotonic: it starts when
the sensor first sees a person, so it cannot be knocked out by a clock jump.

`GET /rooms/1` returns `{room_id, state, holder?, expires_at?, next_res_start?}`
with epoch seconds. Elapsed-on-display is local and monotonic: it starts when
the sensor first sees a person, so it cannot be knocked out by a clock jump.

## Wall clock

The RP2040s have no trustworthy clock, and the API speaks epoch seconds. The
C6 reads the `Date:` header of any API response (free, no extra endpoint, and
the proxy passes it) and sends that epoch over the link, so main can stamp an
elapsed time and the display can render Eastern time. No SNTP dependency,
nothing to punch through the proxy.

## Links

### C6 ↔ main (UART1, JSON lines)

One JSON object per line, `\n` terminated, 115200 baud. The main CPU drains
UART1 every millisecond so a back-to-back `net` / `status` / `time` burst stays
below the RP2040 UART's 32-byte FIFO depth.

Main → C6:

```json
{"t":"presence","v":true}
{"t":"presence","v":false}
{"t":"boot","fw":"roomnode-main","ver":"001"}
{"t":"hb","up_s":42,"present":true}
```

C6 → main:

```json
{"t":"status","state":"reserved","holder":"","url":"https://.../r/1","expires_at":1763200000,"next_res_start":1763203600}
{"t":"cmd","servo":0}
{"t":"alert","text":"Reservation ends in 5 min","level":1}
{"t":"time","epoch":1763199000}
{"t":"net","up":true,"api":"ok"}
```

Rules: unknown keys ignored; unknown `t` ignored; a malformed line is dropped
(never fatal); the main CPU assumes the last `cmd`/`alert` stands until replaced.

### main ↔ display (UART0, `fwog_link`)

Fixed-size packed frame (`room_frame_t`, `apps/roomnode/room_state.h`), pushed
on change and as a 500 ms heartbeat — 100 ms while either presence timer is
running, because a 600 ms window sampled at 2 Hz is a single frame — plus a
display → main action frame for the End button and the servo exerciser. The
exerciser's servo tests move the horn directly, so leaving the menu sends a
`SERVO_RESTORE` action: main re-applies the last position the service commanded,
which nothing else would do, because the C6 only sends a command when the
service's own value changes:

```c
typedef struct __attribute__((packed)) {
    uint8_t  version;       /* ROOM_STATE_VERSION, currently 5 */
    uint8_t  state;         /* room_state_t */
    uint8_t  present;       /* sensor says a body is here */
    uint8_t  net_up;        /* C6 could reach the API */
    uint8_t  alert_level;   /* ROOM_ALERT_* */
    uint8_t  sensor_ok;     /* 0 = sensor did not identify itself */
    uint16_t reserved0;
    uint32_t elapsed_s;     /* since presence began, 0 when absent */
    int32_t  hold_left_s;   /* negative = unknown/not applicable */
    int64_t  epoch;         /* UTC second, 0 until the API Date sync */
    int16_t  tpresence;     /* raw sensor metrics, for tuning on the panel */
    int16_t  tamb_raw;
    int16_t  tpres_delta;   /* excursion above the tracked empty-room floor */
    uint8_t  func_status;   /* PRES_FLAG | MOT_FLAG | TAMB_SHOCK_FLAG */
    uint8_t  pres_phase;    /* room_pres_phase_t: steady | entering | exiting */
    uint8_t  pres_percent;  /* 0..100 through that window, 0 when steady */
    char     alert[48];     /* reason text, empty when none */
    char     holder[32];
    char     qr_url[192];   /* service URL; empty draws no QR */
} room_frame_t;
```

`version` and the payload length are checked before a byte is trusted: a
mismatch between two separately-flashed halves must show as "wrong firmware",
never as garbage on the panel.

### The corner pie

`pres_phase`/`pres_percent` drive a 14 px sector in the bottom-right corner:
green while main is inside the 600 ms confirmation window (it may conclude
somebody arrived), amber while it is inside the 10 s absence hold (it may
conclude the room emptied). Both windows are otherwise invisible — a panel that
looks the same at 0.1 s and 9.9 s into a hold gives nobody anything to tune or
to trust.

Who decides *when* a timer is worth showing is main, because main owns the
timers:

- The absence hold runs from the last motion sample, so it is at 0% for a second
  after somebody arrives. main does not report it until it is
  `PRESENT_PIE_QUIET_MS` (1 s) old, so an amber sector always means the countdown
  has actually started, and a person standing in the room never sees one.
- Confirmation lands on the sample where the verdict flips, one frame short of
  full. main holds the entry phase at 100% for `PRESENT_CONFIRM_HOLD_MS` (300 ms)
  so the sector finishes instead of vanishing at 83%.

The display only bridges gaps: it paints whatever live phase it is handed, and
erases 600 ms after the last one, which absorbs a hold restarting from zero
without ever latching over a live phase. The URL line is capped at 46 columns to
keep the corner clear.

## API contract

The device talks to the **Room Lock API** with `x-device-key`, not Cognito
bearer tokens: the device is provisioned with a key, so there is no login flow
to run. Base URL, room id and the key are `#define`s in the C6 firmware
(`esp32-roomnode/main/roomnode_config.h`).

```c
#define API_BASE   "https://6526ig971l.execute-api.us-east-2.amazonaws.com"
#define ROOM_ID    "1"
#define DEVICE_KEY "<provisioned>"
```

Calls the device makes:

| Call | When |
| --- | --- |
| `GET /device/rooms/1` | every 1 s; this is the heartbeat |
| `POST /device/rooms/1/presence` `{"present":bool}` | on a local belief change, and every 5 s |
| `POST /device/rooms/1/end` | only from the display's End action, and only in `held`/`warn` |

The GET response is the whole device vocabulary — `state`, `url`, `holder`,
`expires_at`, `next_res_start`, `presence`, and provisionally `servo` and
`alert`. Presence is an **observation**, not a claim: a presence edge never
implies claim, check-in, end or booking. Local presence and the service's own
`presence` field are both logged, and a disagreement is reported, not resolved
locally. Field-by-field detail, and the list of assumptions still open with the
API team, are in `roomnode-api-handoff.md`.

Errors arrive as plain text. The device logs the body verbatim and keeps the
last known state, so a failed poll shows as an offline indicator, never as an
invented `free`.

CORS is the web app's problem, not the device's — the device sends no `Origin`.

## Proxy

`#define USE_PROXY 1` routes every request through `192.168.49.1:8228`. IDF
v6.1 has no proxy support in `esp_http_client` *or* `esp-tls`, and no
`esp_tls_conn_new_socket`, so the client is hand-rolled: plain TCP to the
proxy, `CONNECT host:443 HTTP/1.1`, then Mbed TLS over that socket with the
ESP-IDF certificate bundle as the verifier.

That is not only a bench convenience. The tether on that subnet hands out no
usable resolver, so going through the proxy — which resolves names itself — is
what makes DNS work at all. A deployment with real DNS sets `USE_PROXY 0`.

## Open questions

Resolved: the sensor is confirmed on I2C0 at 0x5A (`WHO_AM_I` = `0xD3`), the
backend takes presence as an observation, the servo is a `0|1` command field,
and the QR destination arrives as `url` on every room GET.

Still open:

1. Servo travel and safe angles once the real lock is attached. Today `0|1`
   maps to 0% / 90% of a simulated travel on GPIO27.
2. A production device key — per device, per room, or fleet-wide — and how it
   is provisioned or rotated.
3. Whether `alert` is a real field on device responses; the path is implemented
   but has never been fed by the service.

## Verification status

Running end to end on hardware: Wi-Fi, DHCP, proxy `CONNECT`, TLS validation,
authenticated GET, presence POST, C6 → main `status`/`net`/`time` delivery,
main → display frames, the service-driven UI, and the QR.

Not yet verified: button-driven End through all three CPUs, the `held` /
`reserved` / `warn` transitions against the real service, the `servo` and
`alert` service fields, and beep audibility in a normal room. The per-item list
is in `roomnode-api-handoff.md`.
