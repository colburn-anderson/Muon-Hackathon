# Room node — architecture and logic

How the room node is put together, and why. This is the shape of the thing, not
a code walkthrough: the state machine, the filters, what crosses which wire, and
what stops each part from lying when another part fails. For the wire-level
contracts see `docs/roomnode.md`; for what the backend team needs to confirm see
`docs/roomnode-api-handoff.md`.

## 1. The shape, in one picture

```
   STHS34PF80 (I2C0)          servo (GPIO27)           ST7789 320x240
         |                         |                         |
         +------------+------------+                         |
                      |                                      |
              +-------+--------+                    +--------+---------+
              |  MAIN RP2040   |  packed binary     |   DISPLAY RP2040 |
              |  decides       |  frame, 100-500 ms |   draws only     |
              |  everything    +------------------->|                  |
              +-------+--------+                    +------------------+
                      | JSON lines, 115200 baud
                      v
              +-------+--------+
              |  ESP32-C6      |  HTTPS, 1 Hz
              |  network only  +------------------>  Room Lock API
              +----------------+                      (AWS)
```

Three processors, and the split is not about performance. It is about what each
one is allowed to believe.

## 2. Division of tasks

| CPU | Owns | Refuses |
|---|---|---|
| **Main RP2040** | The presence verdict, the room state machine, the servo, the clock, the alert text, the display's firmware image | Talking to the network |
| **Display RP2040** | Pixels. Every region of the panel, the QR code, the speaker, the buttons | Any decision, any timing policy, any claim about the room |
| **ESP32-C6** | Wi-Fi, TLS, the API client, the proxy tunnel, reporting presence, forwarding commands | Any opinion about whether somebody is in the room |

The useful invariant is the middle line. **The display cannot disagree with the
door**, because it is not allowed an opinion: every pixel it shows is a claim
main made, arriving in one frame. If the panel says FREE, main said free, and the
same value drove the servo. There is no second estimator to drift.

The C6 is symmetric but in the other direction: it has no sensor. It knows how
long a person has been there only because main tells it, and it re-learns that
after either CPU reboots (section 5.4). Its one piece of state is "what I last
successfully told the backend", which is what makes the resend logic possible.

## 3. The data path, end to end

```
IR sample (5 Hz)
  -> sensor's own presence algorithm          FUNC_STATUS flags
  -> empty-room baseline tracker              filtered excursion, counts
  -> excursion threshold                      "is this a body right now"
  -> temporal agreement (600 ms / 10 s)       debounced verdict
  -> room state machine + service status      what the room is
  -> one frame per 100-500 ms                 binary, to the display
  -> published to the API                     on change, and every 5 s
```

Latency from a person walking in to the panel changing is bounded by the
confirmation window (600 ms) plus the frame period (100 ms while a transition is
pending). Latency from a backend change (a reservation appearing) to the panel is
bounded by the poll period (1 s) plus the same frame delay. Neither path
interrupts the other; there is no shared queue, only scheduled work in one loop.

## 4. Presence estimation

This is the hard part of the device and the place where the first two attempts
were wrong, so it is worth describing the reasoning rather than the settings.

### 4.1 What the signal actually is

`TPRESENCE` is not a temperature. It is a filtered low-frequency IR signal from
an uncalibrated part, and its DC offset moves with what is in the field of view,
how the board is angled, self-heating, and ambient drift. Measured on this desk
within one hour, the same empty chair read anywhere from -6300 to -2700, and the
sensor pointed at the ceiling read about +3000. **Any absolute threshold is wrong
within minutes**, which is exactly how the first version failed: it looked
correct for ten minutes and then declared a permanently occupied room.

So nothing downstream is allowed to compare against a constant.

### 4.2 Four filters in series

**1. The sensor's own algorithm.** The part contains a presence and motion
detector with its hysteresis already tuned, exposed as flags in `FUNC_STATUS`.
Using it costs one byte per sample and inherits the silicon's answer to a problem
the part exists to solve. Thresholding the raw value ourselves would inherit
every drift problem the embedded algorithm was written to absorb.

**2. An empty-room floor tracker.** The driver keeps a moving estimate of the
"nobody here" level and reports only the **excursion above it**. The tracker is
deliberately asymmetric:

- it follows **downwards fast** (a quarter of the gap per sample), because an
  empty room must be recognised within seconds, and
- it creeps **upwards very slowly** (1/1024 of the gap per sample, roughly
  3 counts/s at 2 Hz), because a person sitting still must not be quietly
  absorbed into the baseline and then declared absent.

The asymmetry is the whole design: about a quarter of an hour to swallow a real
occupant, far longer than anyone sits in a meeting room without moving, against
seconds to notice the room emptied. The verdict is then simply *excursion above
the floor*, which is offset-free by construction.

**3. A threshold, on the filtered quantity.** Presence requires the excursion to
exceed a fixed number of counts. This is the one constant in the chain, and it is
meaningful only because filter 2 removed the offset: it now means "this much
brighter than this room's own floor", not "this much above zero".

**4. Temporal agreement, in both directions.** One sample is not evidence. The
verdict needs **three consecutive samples (600 ms)** above threshold before it
claims somebody is present, and it takes **10 s of continuous quiet** before it
claims they left. Both edges are debounced, asymmetrically and on purpose:

- Believing presence too eagerly costs a room that occasionally thinks it is
  occupied. Cheap.
- Believing absence too eagerly ends a reservation for someone who was sitting
  still reading. Expensive, and the user sees it.

Hence: fast in, slow out.

### 4.3 What has no evidence

If the sensor stops answering, the node does **not** publish "empty". It holds
the last belief, flags `sensor_ok = 0`, and the panel says the sensor is off.
Claiming an empty room on no evidence is the one failure mode worth spending
pixels to avoid, because absence is what ends a session on the backend.

### 4.4 Known limits, stated honestly

- The excursion threshold is one number fitted to one desk. Moving the sensor
  changes the scale of the signal, not just its offset.
- The part reports `tobj_comp == tobj_raw` on our configuration, so ambient
  compensation is not actually active; ambient and object temperatures are
  reported side by side purely as diagnostic evidence.
- Sitting still is genuinely ambiguous with this sensor, which is why the hold is
  10 s and not 60 s: the node is designed to be re-triggered by ordinary fidgeting
  rather than to be a reliable "still person" detector.

## 5. Communication structure

Three links, three formats, chosen for what fails on each.

### 5.1 C6 -> API: HTTPS, 1 Hz

The C6 speaks to the Room Lock API with a device key, not a user login. Requests
are hand-rolled on top of a raw socket rather than an HTTP client, because the
build target has no proxy support and the bench network has no egress:

```
TCP to proxy -> "CONNECT host:443" -> TLS over that socket -> request
```

- TLS is **verified, not skipped**: certificate bundle, verify required. The
  proxy is a tunnel, not a trust boundary.
- One request per poll, 1 s apart. The poll is not just a heartbeat: it is the
  only channel by which servo and alert commands arrive, so it sets the latency
  of every service-driven behaviour. A dropped poll is retried next second.
- Response bodies are parsed for both fixed-length and chunked encodings,
  because "chunked" is one server-side config change away and the alternative is
  a silently empty body.
- Sockets are opened per request rather than kept alive: simpler to reason about,
  and at 1 Hz the handshake cost is invisible.

### 5.2 C6 <-> main: JSON lines, 115200

The inter-CPU link is deliberately **human-readable and human-typed**: JSON lines
at 115200 baud, one message per line. During bring-up this is what made a
half-working system debuggable from a terminal, and it is why the protocol has
survived three rewrites of both ends.

- Each line is a tag plus fields: status, presence, boot, heartbeat, servo
  command, alert, time, network state.
- **Unknown tags and unknown keys are ignored**, so either CPU can be flashed
  ahead of the other without a lockstep upgrade.
- An over-long line is **dropped whole**, never parsed partially: a truncated
  JSON buffer is how a parse bug becomes a wrong claim.
- Logs never share this wire. The C6's console is on its own USB port precisely
  so one log line cannot corrupt a message.

### 5.3 main <-> display: one packed frame

The display link is a **fixed-size packed binary frame** carrying everything the
panel shows: state, presence, elapsed time, hold countdown, clock, alert, holder,
URL, raw sensor numbers for the diagnostics screen, and the presence-timer phase
and percentage that drive the corner pie.

- **Version and length are both checked before a byte is trusted.** Two
  separately-flashed halves must present as "wrong firmware" on the panel, never
  as plausible garbage.
- Pushed on change and as a 500 ms heartbeat; **100 ms while a presence timer is
  running**, because a 600 ms window sampled at 2 Hz is a single frame and not a
  progress indicator.
- The reverse direction carries only actions (end reservation, servo test) and
  is version-stamped the same way.

### 5.4 Reboots are normal, not exceptions

Either CPU can reboot without the other noticing badly:

- main heartbeats its presence belief every 5 s, and the **C6 rebuilds its belief
  from that heartbeat** after either side restarts. Without it, a main reboot
  leaves the backend convinced of a stale occupancy.
- Hardware state dies with the CPU too, and the C6 caches what it has already
  told main to avoid repeating itself. On main's boot line those caches are
  invalidated and the service state is re-sent on the next poll. The servo is the
  one that matters: main comes up locked, so with a cache that still matched, a
  service asking for *unlocked* would leave the door locked until it happened to
  change its mind. The same class of divergence is why leaving the device
  exerciser re-asserts the service's last servo position.
- The C6 reports presence on **change and again every 5 s**. One dropped POST
  must not be able to look like an empty room, because absence is what ends the
  session.
- main owns the display's firmware: at boot it checks the panel's image against
  the one embedded in its own UF2, transfers it if they differ, and only then
  starts running. That is why a display app is never flashed directly — the two
  halves cannot drift out of sync, they are shipped as one artifact.

## 6. Presentation

The display is the only place where the temptation to add policy is real, and it
is resisted on purpose: it holds no timers, no thresholds and no state about the
room. Its job is to paint the frame it was handed, cheaply.

- **No full-screen clear after the first paint.** Every region owns its rectangle,
  paints its own background, and redraws only when the value it shows changes. A
  clear-and-repaint on each frame flickers the whole panel, which is worse than
  useless on a display people glance at from across the room.
- **The panel says when it is guessing.** A status display whose link has died
  looks exactly like a status display whose room is free, so the two facts that
  can be stale — "the service said this" and "the sensor is answering" — each
  own a piece of the panel: an amber band for a dead link (and `net up` expires
  on fifteen seconds of silence, so the band cannot be defeated by a wire that
  died mid-sentence), and `presence unknown` instead of a held verdict when the
  sensor stops answering. The bottom hint is contextual for the same reason: it
  names the action available in this state, and offers none while offline,
  because End travels through the C6.
- **The QR code is encoded only when the URL changes.** Encoding allocates two
  ~1 KB buffers and runs a Reed-Solomon pass; doing that per refresh on a machine
  with no FPU and a panel to feed is a visible stall.
- **The presence pie is debounced at the source, not at the display.** An early
  version kept a dwell timer in the display and latched the phase it had drawn;
  because a hold that restarts from zero on a stray sample never satisfies a
  dwell, the sector froze part-filled, and a settled room kept an empty ring on
  screen. The judgement moved to main, which owns the timers: it does not report
  the 10 s absence countdown until it is a second old (so an amber sector always
  means the countdown has started, and a person in the room never sees one), and
  it holds the 600 ms confirmation at 100% for 300 ms so the sector visibly
  completes. The display now only bridges gaps: it paints any live phase, and
  erases 600 ms after the last one.

## 7. Safeguards

Failure by failure, what the system does:

| Failure | Behaviour |
|---|---|
| App hangs | main runs the hardware watchdog and kicks it every loop iteration; the display deliberately has **none** — a frozen picture is better than a reboot loop on a panel nobody is touching |
| Sensor unplugged or NAKing | belief held, `sensor_ok = 0`, panel shows `sensor OFF`; never publishes "empty" on no evidence |
| Wi-Fi drops | reconnect after a deliberate delay rather than immediately, so an AP that rejects us cannot turn the retry into a spin |
| API unreachable | **fail secure**: after 60 s without a successful poll the C6 commands the servo locked, independent of what the display shows |
| C6 reboots | belief restored from main's heartbeat; presence re-posted on the next change or reminder |
| main reboots | display image re-verified and transferred if needed before the app runs |
| Firmware halves disagree | frame version/length mismatch renders as "wrong firmware" on the panel |
| Malformed or over-long message | dropped whole, logged, never partially applied |
| Servo value out of range | clamped, so a backend bug cannot drive the horn into its stop and stall the motor |
| Power button | the red button's 6 s hold is the only way to power down; both app classes use the shared power helper, and the display polls it every loop |
| Proxy unreachable | request fails, poll retries, panel keeps the last known service state and shows the network as down |

## 8. Timing, in one place

| Interval | Value | Why |
|---|---|---|
| Sensor sample | 200 ms | 5 Hz: a room-scale IR part reports no faster, and 5 Hz makes the 600 ms debounce three honest samples |
| Presence confirm | 600 ms | three consecutive samples; one stray sample must not check somebody in |
| Absence hold | 10 s | long enough to survive reading a page, short enough that an empty room is reported while the next person is still walking in |
| Baseline creep up | 1/1024 per sample | ~3 counts/s: ~15 min to absorb an occupant |
| Baseline follow down | 1/4 per sample | seconds to recognise an empty room |
| main -> display frame | 500 ms idle, 100 ms while a transition is pending | a 600 ms window needs more than one frame |
| main -> C6 heartbeat | 5 s | belief repair, not liveness theatre |
| C6 -> API poll | 1 s | it is the latency of every service-driven behaviour |
| Presence re-post | on change, and every 5 s | silence must not read as an empty room |
| Offline forced lock | 60 s without a successful poll | fail secure |
| Pi: phase entry hold / exit gate / erase bridge | 300 ms / 1 s / 600 ms | finish the sector, do not cry wolf, do not flicker |

## 9. What is deliberately not here

- **No local occupancy model.** The node does not keep a schedule or a
  reservation copy. It asks the service what the room is and renders the answer;
  a reservation created on the web appears within a poll period.
- **No claim/check-in flow.** The device reports presence as an *observation*, not
  as a booking command. Presence edges never imply claim, check-in or end.
- **No servo feedback.** Open loop: two angles, 1-2 ms pulses at 50 Hz, and the
  code never claims more than "we are commanding locked". Note that GPIO26/27 are
  FPGA-side pads on this board, so if the pulse is provably being generated and
  the servo does not move, suspect the pad routing before the servo code.
- **No display-side watchdog**, for the reason in section 7.
- **No proxy support in the firmware's dependencies.** The tunnel is hand-rolled
  in `net.c`; the shipped TLS stack and HTTP client on this IDF version expose no
  proxy fields at all. It is optional at build time (`USE_PROXY`) because it only
  exists for bench networks with no egress.
