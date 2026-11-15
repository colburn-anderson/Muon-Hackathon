# `esp32-roomnode` — the C6 network side of the room node

Standalone ESP-IDF project for the ESP32-C6 on the Bottlenose Orca. It owns the
network and nothing else: it publishes what the main RP2040 senses and forwards
what the backend commands.

Design and the cross-CPU contract: [`../docs/roomnode.md`](../docs/roomnode.md).

## What it does

- Wi-Fi station, retrying forever.
- **Link to the main CPU** on UART0 (GPIO16 TX / GPIO17 RX, 115200 baud,
  JSON lines — see `roomnode_config.h`).
- **Polls `GET {API_BASE}/device/rooms/1`** every `POLL_MS`, forwarding
  `state`, `url`, `expires_at`, and `next_res_start` to the main CPU. A `servo`
  target or `alert` banner is also forwarded if the backend supplies one.
- **Reports observed presence** as a boolean on each transition and every 60
  seconds. The service, not the device, decides what that observation means.
- **Wall clock**: the `Date:` header of any response becomes an epoch the main
  CPU can use, because neither RP2040 has a trustworthy clock.

Console is on **USB Serial/JTAG** (the ESP32's own USB port), deliberately not
on UART0 — UART0 is the 115200-baud protocol wire, and one log line on it would
corrupt a JSON message.

## Configure

Everything lives in `main/roomnode_config.h`:

| Define | Meaning |
| --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | station credentials |
| `USE_PROXY` | 1 = route everything through `PROXY_HOST:PROXY_PORT` (bench default), 0 = direct |
| `API_BASE` | HTTPS API gateway base URL |
| `DEVICE_KEY` | value sent in `x-device-key` |
| `ROOM_ID` | `"1"` |
| `POLL_MS` | room-state poll interval |

### The proxy

`USE_PROXY 1` is the **bench default**: this network has no direct route out.
`net.c` opens an HTTP `CONNECT` tunnel to the proxy and then performs normal TLS
to the API host. The proxy also resolves the hostname because the tether does
not provide a working resolver.

Set `USE_PROXY 0` on a network with direct DNS and HTTPS egress.

### Device credential

The room fixture authenticates with `x-device-key`, not a Cognito user token.
The development default is replaceable from the ignored
`main/roomnode_secrets.h`; production provisioning and rotation remain an API
team decision.

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

**Address the C6 by its USB identity, never by `ttyACM` number.** The three
serial devices on this board renumber themselves on every replug, and writing
esptool frames into the wrong one fails in the most misleading way available:
the RP2040's USB CDC accepts the bytes and the flash tool sits waiting for a
response that will never come, reporting "invalid segment length 0xffffffff",
write timeouts, or an erase that never completes. It reads exactly like a dead
board. It is not.

```sh
export C6=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_E4:B3:23:A6:06:5C-if00
CCACHE_DISABLE=1 idf.py -B build -p "$C6" flash
idf.py -B build -p "$C6" monitor
```

Check with `ls /dev/serial/by-id/` first: the C6 is `usb-Espressif_USB_JTAG_*`,
the two RP2040s are `usb-FreeWili_OG_FWOG_{main,display}_*`. If the C6 identity
is missing, the board is not the problem you think it is.

A healthy boot:

```
I (312) roomnode: room 1 via https://6526ig971l.execute-api.us-east-2.amazonaws.com (proxy 192.168.49.1:8228)
I (318) link: link up: uart0 tx=16 rx=17 115200 baud
I (560) wifi: got ip:192.168.49.194 gw:192.168.49.1 mask:255.255.255.0
I (570) wifi: dns main:192.168.49.1
I (610) link: main CPU booted: roomnode-main
I (3358) api: GET /device/rooms/1 -> 200 free
```

The first request can fail with `connect(...) failed: errno 118` if the API
loop starts before DHCP finishes. That is expected and self-correcting: the
next poll, five seconds later, succeeds.

## Tests

The JSON reader (`main/jhelp.c`) is plain C with no IDF headers, so it is
tested from the host tree instead of by flashing:

```sh
cd .. && python3 tools/fw.py test        # 41 CTest binaries + 197 fw.py tests
```

`tests/test_jhelp.c` asserts the real room GET body parses, that a key
appearing only as another key's *value* does not match, that a nested `alert`
object neither leaks into the top level nor gets lost, and that truncated
buffers and malformed input fail instead of overrunning.

## Verification status

Observed on hardware: Wi-Fi association and DHCP, the proxy `CONNECT` tunnel,
TLS validation against the bundle, an authenticated `GET /device/rooms/1`
answering `200`, a presence POST answering `200`, `Date`-header clock sync, and
`status` / `net` / `time` frames arriving on the main CPU.

Not verified: the `servo` and `alert` response fields (the service has not
emitted them), `/end` against the live service, and the USB mock console
commands. See `../docs/roomnode-api-handoff.md`.
