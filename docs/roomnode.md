# Room presence / reservation node — architecture

One fixed room, `room_id = 1`. The FreeWili OG is a **simple instruction
follower**: it senses the room, reports it, and obeys what the backend tells it.
All policy lives in the AWS API; the device is not allowed to invent state.

Status: design. Nothing below is implemented yet except the C6's Wi-Fi and
proxy plumbing, which came through the CSI work.

## Hardware map

| Thing | Where | Notes |
| --- | --- | --- |
| STHS34PF80 presence sensor | **main CPU breakout I2C0, SDA GPIO16 / SCL GPIO17** | replaces the WiFi-CSI experiment entirely |
| Door-lock servo | **main CPU breakout GPIO27** | PWM, open-loop; not attached yet |
| ESP32-C6 | on the Bottlenose Orca | Wi-Fi + HTTPS to the API |
| ESP32 ↔ main link | main CPU **UART1, GPIO8 TX / 9 RX / 10 CTS / 11 RTS**, 3 Mbaud 8N1, RTS/CTS | proven by `apps/espuart` |
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
- QR encodes `https://main.d24j7edplzktpl.amplifyapp.com/r/1` — the reservation
  link for this room, straight from the URL the backend handed us. It is
  **not** the API URL and carries no token.
- Buttons are for bring-up (screen cycling); the reservation flow lives in the
  web app the QR points at.

## State model

`state` arrives from the API; the device never computes it.

| State | Display | Servo |
| --- | --- | --- |
| `free` | "Free" + QR to reserve | locked |
| `held` | "Held — check in" + countdown to hold expiry | locked |
| `reserved` | "Reserved 14:30–15:00", next start time | locked |
| `warn` | banner + reason ("Reservation ends in 5 min") | locked/unlocked per command |
| occupied, any state | elapsed `mm:ss` since presence began, instead of the QR | per command |

`GET /rooms/1` returns `{room_id, state, holder?, expires_at?, next_res_start?}`
with epoch seconds. Elapsed-on-display is local and monotonic: it starts when
the sensor first sees a person, so it cannot be knocked out by a clock jump.

## Wall clock

The RP2040s have no trustworthy clock, and the API speaks epoch seconds. Plan:
the C6 reads the `Date:` header of any API response (free, no extra endpoint,
and the proxy passes it) and forwards an offset the main CPU applies to its
monotonic timer. No SNTP dependency, nothing to punch through the proxy.

## Links

### C6 ↔ main (UART1, JSON lines)

One JSON object per line, `\n` terminated, 3 Mbaud. Cheap to parse, easy to
debug with `apps/espuart` style lifting, and no framing state machine to get
wrong at 3 Mbaud.

Main → C6:

```json
{"t":"presence","v":true}
{"t":"presence","v":false}
{"t":"boot","fw":"roomnode-main","ver":"001"}
{"t":"hb","up_s":42,"present":true}
```

C6 → main:

```json
{"t":"status","state":"reserved","expires_at":1763200000,"next_res_start":1763203600}
{"t":"cmd","servo":42}
{"t":"alert","text":"Reservation ends in 5 min","level":"warn"}
{"t":"time","epoch":1763199000}
{"t":"net","up":true,"api":"ok"}
```

Rules: unknown keys ignored; unknown `t` ignored; a malformed line is dropped
(never fatal); the main CPU assumes the last `cmd`/`alert` stands until replaced.

### main ↔ display (UART0, `fwog_link`)

Fixed-size state frame, pushed on change and as a 1 Hz heartbeat:

```c
typedef struct {
    uint8_t  state;        /* FWOG_ROOM_FREE|HELD|RESERVED|WARN */
    uint8_t  present;      /* sensor */
    uint8_t  net_up;
    uint8_t  alert_level;  /* 0 none, 1 warn, 2 urgent */
    uint16_t elapsed_s;    /* since presence began, 0 when absent */
    int32_t  hold_left_s;  /* countdown to hold/ reservation expiry */
    char     alert[48];    /* reason text, empty when none */
    char     qr_url[96];   /* reservation link for the QR */
} fwog_room_state_t;
```

## API contract (placeholder until the real endpoints land)

Base URL is a `#define` in the C6 firmware:

```c
#define API_BASE "https://api.example.invalid/v1"   /* placeholder */
#define ROOM_ID  "1"
#define QR_URL   "https://main.d24j7edplzktpl.amplifyapp.com/r/1"
```

Calls the device makes:

| Call | When |
| --- | --- |
| `POST {base}/rooms/1/claim` | presence begins and the room is `free` |
| `POST {base}/rooms/1/checkin` | presence continues past the hold window |
| `POST {base}/rooms/1/end` | presence absent for N seconds |
| `GET {base}/rooms/1` | every poll, always (this is the heartbeat) |
| `GET {base}/reservations?mine=true` | only if the display needs the next reservation label |

Auth: `Authorization: Bearer <Cognito ID token>`. The device cannot run a
Cognito login flow, so the token is **provisioned** — a `#define` placeholder
for now, replaced later by a short-lived token pushed over the same UART link
(or by a device-credentials flow the backend adds). Note the issuer
`https://cognito-idp.us-east-2.amazonaws.com/us-east-2_SDmtOUvtp`, audience
`5bo2ki57dkj6djpvr3t86cgt80`.

Errors arrive as plain text; the device logs the body verbatim and keeps the
last known state. `401` puts the display in a "not logged in" banner, which is
the only auth failure a human can act on.

CORS is the web app's problem, not the device's — the device sends no `Origin`.

## Optional proxy

`#define USE_PROXY 0` in the C6 firmware. Set to 1 and everything routes through
`192.168.49.1:8228` as an absolute-URI `GET`/`POST` on a raw socket (IDF v6.1's
`esp_http_client` has no proxy fields; see the reference copy of
`sockets_component.h` in `../work/keep/`). TLS through a proxy needs a CONNECT
tunnel — implement that only if the demo network forces it.

## Open questions

1. Sensor confirmation: read `WHO_AM_I` (STHS34PF80 is `0xD3` at 0x5A on the
   usual wiring) over I2C0 before writing the driver. Cheap, and it also proves
   the 16/17 wiring.
2. Does the backend want `presence` as its own resource, or is `claim`/
   `checkin`/`end` the whole vocabulary? The four calls above are a guess at
   the intent, and one extra endpoint would be cheaper than mimicking a user.
3. Servo travel and safe angles (locked/open) once the hardware exists.
4. Token provisioning path — this is the one thing that cannot be faked well.
