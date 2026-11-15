# `esp32-roomnode` — the C6 network side of the room node

Standalone ESP-IDF project for the ESP32-C6 on the Bottlenose Orca. It owns the
network and nothing else: it publishes what the main RP2040 senses and forwards
what the backend commands.

Design and the cross-CPU contract: [`../docs/roomnode.md`](../docs/roomnode.md).

## What it does

- Wi-Fi station, retrying forever.
- **Link to the main CPU** on UART0 (GPIO16 TX / GPIO17 RX, 3 Mbaud, JSON lines
  — see `roomnode_config.h`). RX only, in practice: presence in, commands out.
- **Polls `GET {API_BASE}/rooms/1`** every `POLL_MS`, forwards `state`,
  `expires_at`, `next_res_start` to the main CPU when they change, and forwards
  a `servo` angle or an `alert` banner when the backend sends one.
- **Publishes presence**: `claim` when a body appears in a free room,
  `checkin` when one appears in a held/reserved room, `end` after
  `ABSENT_TIMEOUT_S` of no presence.
- **Wall clock**: the `Date:` header of any response becomes an epoch the main
  CPU can use, because neither RP2040 has a trustworthy clock.

Console is on **USB Serial/JTAG** (the ESP32's own USB port), deliberately not
on UART0 — UART0 is the 3 Mbaud protocol wire, and one log line on it would
corrupt a JSON message.

## Configure

Everything lives in `main/roomnode_config.h`:

| Define | Meaning |
| --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | station credentials |
| `USE_PROXY` | 1 = route everything through `PROXY_HOST:PROXY_PORT` (bench default), 0 = direct |
| `API_BASE` | **placeholder** `http://api.example.invalid/v1` until the real gateway exists |
| `API_TOKEN` | **placeholder** Cognito ID token — provisioned, see the caveat below |
| `ROOM_ID` | `"1"` |
| `QR_URL` | the reservation link the display encodes |
| `POLL_MS`, `ABSENT_TIMEOUT_S` | heartbeat period, and how long "empty" must last |

### The proxy

`USE_PROXY 1` is the **bench default**: this network has no direct route out.
Requests become absolute-URI `GET http://host:port/path` on a raw socket to the
proxy, which is what a forward proxy expects.

Only `http://` is proxied this way. An `https://` API through a proxy needs a
`CONNECT` tunnel, which this client does not implement — it logs
`only http:// is supported` and fails visibly rather than hanging. That is a
deliberate gap for the placeholder stage; fill it when the real AWS host is
known, or run the device on a network with a direct route.

### The token caveat

`API_TOKEN` is a compile-time placeholder. The device cannot run a Cognito
login flow, so the token has to be **provisioned** — pushed over the UART link
from the main CPU, or issued by a device-credentials grant the backend adds.
Until then, every call carries the placeholder string and the API answers 401.
That is expected, and the display shows it as a "not logged in" banner rather
than pretending the room is free.

## Build

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
cd esp32-roomnode
CCACHE_DISABLE=1 idf.py -B build set-target esp32c6   # once
CCACHE_DISABLE=1 idf.py -B build build
```

`CCACHE_DISABLE=1` is not superstition: ccache has already served a stale
object for a changed source file once in this project, and the flashed binary
simply did not contain the change, with a clean build log.

## Flash

```sh
CCACHE_DISABLE=1 idf.py -B build -p /dev/ttyACM2 flash
idf.py -B build -p /dev/ttyACM2 monitor
```

`/dev/ttyACM2` is the C6's own USB-Serial/JTAG port (`usb-Espressif_USB_JTAG_*`
in `/dev/serial/by-id/`). No button, no bootloader dance.

**Do not let anything assert both DTR and RTS on this port** — on the C6's
USB-Serial/JTAG that is the esptool download strap, and the board reboots into
the bootloader instead of running. A raw pyserial reader must set
`s.dtr = False; s.rts = False`.

A healthy boot:

```
I (312) roomnode: fwog room node: room 1, qr https://main.d24j7edplzktpl.amplifyapp.com/r/1
I (312) roomnode: api http://api.example.invalid/v1 (via proxy 192.168.49.1)
I (318) link: link up: uart0 tx=16 rx=17 3000000 baud
I (560) wifi: got ip:192.168.49.194
I (610) link: main CPU booted: roomnode-main
W (2600) api: GET .../rooms/1 -> 401 PLACEHOLDER-COGNITO-ID-TOKEN
```

The 401 is the token placeholder doing what it says on the tin.

## Tests

The JSON reader (`main/jhelp.c`) is plain C with no IDF headers, so it is
tested from the host tree instead of by flashing:

```sh
cd .. && python3 tools/fw.py test        # 41 CTest binaries + 197 fw.py tests
```

`tests/test_jhelp.c` asserts the real `GET /rooms/1` body parses, that a key
appearing only as another key's *value* does not match, that a nested `alert`
object neither leaks into the top level nor gets lost, and that truncated
buffers and malformed input fail instead of overrunning.

Nothing in this project has been observed on hardware yet: the board dropped
off USB during bring-up and had not re-enumerated when this was written. Build
verified; behaviour unverified.
