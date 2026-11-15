/* Room node, main CPU: the half that knows things.
 *
 * It owns the presence sensor, the door-lock servo and the wall clock, and it
 * relays what the ESP32-C6 learns from the API to the display. The C6 owns the
 * network and the state machine for claim/checkin/end; the display owns pixels.
 * See docs/roomnode.md.
 *
 * Two hard requirements from the BSP contract, both live here:
 *   - FWOG_WATCHDOG_DEFAULT() and board_watchdog_kick() every loop iteration,
 *     because board_init() arms an 8.3 s watchdog that takes the display down
 *     with it through GUI_NRESET.
 *   - fwog_display_update_run() rather than board_release_display(). */

#include <string.h>

#include "fwog_main.h"
#include "pico/stdlib.h"

#include "esp_link.h"
#include "presence.h"
#include "room_config.h"
#include "room_state.h"
#include "servo.h"

FWOG_WATCHDOG_DEFAULT();

#define FW_VERSION_STR  "001"

/* Presence sampling. The sensor is slow by design (a room-scale IR array does
 * not report a heartbeat); 5 Hz costs nothing and makes the debounce honest. */
#define SAMPLE_MS            200u
#define PRESENT_HOLD_MS      10000u   /* how long one motion event counts for */
#define PRESENT_CONFIRM_MS   600u     /* 3 samples before believing a body */
/* The corner pie is the only thing these two govern. The exit countdown runs
 * from the last motion sample, so it is at 0% for a full second after somebody
 * arrives -- drawing that says "leaving" to a room that just filled up. Only
 * report it once the countdown is visibly under way. And when it completes the
 * entry pie must actually reach 100% before it is replaced: the phase flips to
 * "present" on the sample where confirmation succeeds, which is one frame short
 * of full. */
#define PRESENT_PIE_QUIET_MS 1000u
#define PRESENT_CONFIRM_HOLD_MS 300u

#define DISPLAY_PERIOD_MS    500u
#define DISPLAY_FAST_MS      100u   /* while a presence timer is running */
#define HB_PERIOD_MS         5000u
/* The C6 speaks at least once a second -- a poll attempt, successful or not --
 * so this much silence means the wire or the C6 itself has gone. */
#define LINK_STALE_MS       15000u
#define IO_APPLY_TRIES       20u
#define IO_APPLY_RETRY_MS    50u

typedef struct {
    bool     present;         /* debounced */
    uint32_t present_since_ms;
    uint32_t last_motion_ms;
    uint32_t confirm_since_ms;
    uint32_t confirm_done_ms; /* when confirmation last succeeded, 0 = never */
    bool     confirming;
    bool     sensor_ok;

    /* Freshness of the C6 link: the last "net up" we heard must not outlive the
     * evidence for it, or the panel advertises online while nothing updates. */
    uint32_t last_c6_ms;
    uint32_t last_rx_lines;

    /* The last position the service commanded, kept because the servo tests in
     * the exerciser overwrite it and the C6 only re-sends on a change. */
    int      servo_api_percent;
    bool     servo_api_valid;

    char     alert[48];
    uint8_t  alert_level;

    int64_t  epoch;           /* wall clock estimate, 0 = unknown */
    uint32_t epoch_at_ms;

    /* Last raw reading, kept only so the status line can show it. A presence
     * algorithm that never fires looks identical to a sensor with no data, and
     * these three numbers are what tells the two apart on the bench. */
    uint8_t  func_status;
    int16_t  tpresence;
    int16_t  tpres_delta;
    int16_t  tpres_base;
    int16_t  tamb_raw;
    int16_t  tobj_raw;
    int16_t  tobj_comp;

    /* Frame sends, counted so a link that has silently stopped can be told
     * apart from one the display has stopped reading. */
    uint32_t frames_sent;
    uint32_t frames_fail;
} node_t;

/* One direction word for the whole app. The display, breakout I2C0 and UART1
 * all live in this single cfg, so exactly one entity applies it -- two applies
 * would overwrite each other and the loser's pads would revert to SIO. */
static bool apply_io(void) {
    fwog_io_cfg_t cfg = *fwog_io_dir_last();
    cfg.uart_tx_out  = true;    /* GPIO 8 driven by UART1 TX */
    cfg.uart_rx_out  = false;   /* GPIO 9 reads UART1 RX */
    cfg.uart_cts_out = false;   /* GPIO 10 reads C6 RTS */
    cfg.uart_rts_out = true;    /* GPIO 11 drives C6 CTS */
    cfg.i2c_pullup   = true;    /* breakout I2C0 needs them; the sensor has none */
    cfg.gpio27_out   = true;    /* servo pulse, if the FPGA routes the pad */

    for (unsigned i = 0; i < IO_APPLY_TRIES; i++) {
        const fwog_io_result_t r = fwog_io_dir_apply(&cfg);
        if (r == FWOG_IO_OK) {
            return true;
        }
        board_watchdog_kick();
        sleep_ms(IO_APPLY_RETRY_MS);
    }
    return false;
}

/* ---- wall clock -------------------------------------------------------
 * Neither RP2040 has a battery-backed clock. The API's Date header (relayed by
 * the C6) is the only source of absolute time, so this keeps a base epoch and
 * counts from the monotonic clock: it drifts, and it is only ever used for
 * "how long until this reservation expires". */

static void clock_sync(node_t *n, int64_t epoch) {
    if (epoch <= 0) {
        return;
    }
    n->epoch = epoch;
    n->epoch_at_ms = to_ms_since_boot(get_absolute_time());
}

static int64_t clock_now(const node_t *n) {
    if (n->epoch == 0) {
        return 0;
    }
    const uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - n->epoch_at_ms;
    return n->epoch + (int64_t)(elapsed / 1000u);
}

static void push_display(node_t *n, const esp_link_in_t *in) {
    room_frame_t f;
    memset(&f, 0, sizeof f);
    f.version = ROOM_STATE_VERSION;
    f.state = in->status_valid ? (uint8_t)in->state : (uint8_t)ROOM_UNKNOWN;
    f.present = n->present ? 1u : 0u;

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool link_fresh = n->last_c6_ms != 0u &&
                            now_ms - n->last_c6_ms < LINK_STALE_MS;
    f.net_up = (in->net_valid && in->net_up && link_fresh) ? 1u : 0u;
    f.sensor_ok = n->sensor_ok ? 1u : 0u;
    f.alert_level = n->alert_level;
    f.tpresence = n->tpresence;
    f.tpres_delta = n->tpres_delta;
    f.tamb_raw = n->tamb_raw;
    f.func_status = n->func_status;
    snprintf(f.alert, sizeof f.alert, "%s", n->alert);
    snprintf(f.holder, sizeof f.holder, "%s", in->holder);
    snprintf(f.qr_url, sizeof f.qr_url, "%s", in->qr_url);

    if (n->present) {
        f.elapsed_s = (now_ms - n->present_since_ms) / 1000u;
    }

    /* The two windows in which the verdict is still undecided, as a fraction.
     * Both are invisible otherwise: the panel looks identical a tenth of a
     * second and nine seconds into the absence hold, so there is nothing to
     * watch while tuning, and nothing to tell a person at the door whether the
     * node has noticed them yet. */
    f.pres_phase = (uint8_t)ROOM_PRES_STEADY;
    f.pres_percent = 0u;
    if (n->confirming) {
        const uint32_t spent = now_ms - n->confirm_since_ms;
        f.pres_phase = (uint8_t)ROOM_PRES_ENTERING;
        f.pres_percent = (uint8_t)((spent >= PRESENT_CONFIRM_MS)
                                   ? 100u
                                   : spent * 100u / PRESENT_CONFIRM_MS);
    } else if (n->confirm_done_ms != 0u &&
               now_ms - n->confirm_done_ms < PRESENT_CONFIRM_HOLD_MS) {
        /* One last full frame: the sector finishes rather than vanishing at
         * 83%, which is what "stuck near completion" looked like. */
        f.pres_phase = (uint8_t)ROOM_PRES_ENTERING;
        f.pres_percent = 100u;
    } else if (n->present) {
        const uint32_t quiet = now_ms - n->last_motion_ms;
        if (quiet >= PRESENT_PIE_QUIET_MS && quiet < PRESENT_HOLD_MS) {
            f.pres_phase = (uint8_t)ROOM_PRES_EXITING;
            f.pres_percent = (uint8_t)(quiet * 100u / PRESENT_HOLD_MS);
        }
    }

    /* How long the current hold has left. Negative means "does not apply",
     * which is what the display renders as a dash rather than "0:00". */
    f.hold_left_s = -1;
    const int64_t now = clock_now(n);
    f.epoch = now;
    if (now > 0 && in->status_valid) {
        int64_t deadline = 0;
        if ((in->state == ROOM_HELD || in->state == ROOM_WARN) &&
            in->expires_at > now) {
            deadline = in->expires_at;
        } else if (in->state == ROOM_RESERVED && in->next_res_start > now) {
            deadline = in->next_res_start;
        }
        if (deadline > 0) {
            f.hold_left_s = (int32_t)(deadline - now);
        }
    }

    uint8_t payload[1u + sizeof f];
    payload[0] = ROOM_MSG_STATE;
    memcpy(payload + 1, &f, sizeof f);
    if (fwog_link_uart_send_frame(payload, sizeof payload)) {
        n->frames_sent++;
    } else {
        n->frames_fail++;
    }
}

static fwog_link_rx_t s_display_rx;

/* Display buttons are deliberately commands, not state mutations. Main relays
 * the request to the network CPU; the next API poll remains the source of truth
 * for what state the room actually entered. */
static void poll_display_actions(node_t *n) {
    uint8_t byte;
    while (fwog_link_uart_read(&byte)) {
        size_t len = 0u;
        if (!fwog_link_rx_byte(&s_display_rx, byte, &len)) {
            continue;
        }
        if (len != 1u + sizeof(room_action_t) ||
            s_display_rx.buf[0] != ROOM_MSG_ACTION) {
            continue;
        }

        room_action_t action;
        memcpy(&action, s_display_rx.buf + 1, sizeof action);
        if (action.version != ROOM_STATE_VERSION) {
            continue;
        }
        if (action.action == ROOM_ACTION_END) {
            esp_link_send_end();
            DIAG("[roomnode] end requested from display\n");
        } else if (action.action == ROOM_ACTION_TEST_SERVO_LOCK) {
            servo_set_percent(0);
            DIAG("[roomnode] local test: servo locked (0%%)\n");
        } else if (action.action == ROOM_ACTION_TEST_SERVO_UNLOCK) {
            servo_set_percent(90);
            DIAG("[roomnode] local test: servo unlocked (90%%)\n");
        } else if (action.action == ROOM_ACTION_SERVO_RESTORE) {
            /* Leaving the exerciser. Without this the door stays at whatever
             * the servo test left it at until the service happens to change its
             * own value, which it may never do. */
            if (n->servo_api_valid) {
                servo_set_percent(n->servo_api_percent);
                DIAG("[roomnode] servo restored to api %d%%\n",
                     n->servo_api_percent);
            } else {
                DIAG("[roomnode] servo restore: no api value yet, left at %d%%\n",
                     servo_percent());
            }
        }
    }
}

int main(void) {
    board_init();

    /* Brings the display up: link, HELLO, reflash if needed, RUN. Performs
     * board_release_display() itself, so nothing else may call it. */
    const fwog_display_result_t d = fwog_display_update_run();

    const bool pads_ok = apply_io();
    servo_init();
    const bool link_ok = esp_link_init();
    esp_link_send_boot(FW_VERSION_STR);

    node_t n;
    memset(&n, 0, sizeof n);
    n.sensor_ok = presence_init();

    esp_link_in_t in;
    memset(&in, 0, sizeof in);
    fwog_link_rx_init(&s_display_rx);

    DIAG("[roomnode] fw %s: pads=%d link=%d sensor=%d display=%s\n",
         FW_VERSION_STR, pads_ok, link_ok, n.sensor_ok,
         fwog_display_result_text(d));

    uint32_t next_sample = to_ms_since_boot(get_absolute_time());
    uint32_t next_display = next_sample;
    uint32_t next_hb = next_sample;
    const uint32_t boot_ms = next_sample;

    while (true) {
        board_watchdog_kick();

        const uint32_t now = to_ms_since_boot(get_absolute_time());

        /* Drain the link every iteration: a status line that sits in the FIFO
         * while we sleep is a display that lags the room. */
        esp_link_poll(&in);

        /* Any line at all is proof the C6 is alive; the absence of lines is the
         * only evidence we will ever get that it is not. */
        if (in.rx_lines != n.last_rx_lines) {
            n.last_rx_lines = in.rx_lines;
            n.last_c6_ms = now;
        }
        poll_display_actions(&n);

        if (in.time_valid) {
            clock_sync(&n, in.epoch);
            in.time_valid = false;
        }

        if (in.servo_valid) {
            n.servo_api_percent = in.servo_percent;
            n.servo_api_valid = true;
            servo_set_percent(in.servo_percent);
            DIAG("[roomnode] servo -> %d%% (%d us pulse)\n",
                 servo_percent(), SERVO_US_LOCKED + (SERVO_TRAVEL_US * servo_percent()) / 100);
            in.servo_valid = false;
        }

        if (in.alert_valid) {
            snprintf(n.alert, sizeof n.alert, "%s", in.alert);
            n.alert_level = in.alert_level;
            DIAG("[roomnode] alert(%u): %s\n", n.alert_level, n.alert);
            in.alert_valid = false;
        }

        if (now >= next_sample) {
            next_sample = now + SAMPLE_MS;

            const presence_sample_t s = presence_poll();
            n.sensor_ok = s.ok;
            n.func_status = s.func_status;
            n.tpresence = s.tpresence;
            n.tpres_delta = s.tpres_delta;
            n.tpres_base = s.tpres_base;
            n.tamb_raw = s.tamb_raw;
            n.tobj_raw = s.tobj_raw;
            n.tobj_comp = s.tobj_comp;

            if (s.ok) {
                if (s.motion) {
                    n.last_motion_ms = now;
                    if (!n.present) {
                        /* Requires PRESENT_CONFIRM_MS of agreement before we
                         * claim somebody is here: one stray sample must not
                         * check a person into a room. */
                        if (!n.confirming) {
                            n.confirming = true;
                            n.confirm_since_ms = now;
                        } else if (now - n.confirm_since_ms >= PRESENT_CONFIRM_MS) {
                            n.present = true;
                            n.confirming = false;
                            n.confirm_done_ms = now ? now : 1u;
                            n.present_since_ms = now;
                            esp_link_send_presence(true);
                            DIAG("[roomnode] presence: present\n");
                        }
                    }
                } else {
                    n.confirming = false;
                    if (n.present && now - n.last_motion_ms >= PRESENT_HOLD_MS) {
                        n.present = false;
                        esp_link_send_presence(false);
                        DIAG("[roomnode] presence: absent after %us\n",
                             (unsigned)(PRESENT_HOLD_MS / 1000u));
                    }
                }
            } else if (n.present) {
                /* The sensor fell off the bus while somebody was here. Hold the
                 * belief rather than publishing "empty" on no evidence. */
                n.present = true;
            }
        }

        if (now >= next_display) {
            /* A 600 ms window sampled every 500 ms is one frame, which is not a
             * progress indicator. Push four times as often while either timer
             * is running; the frame is ~300 bytes and the link is idle anyway. */
            const bool timing =
                n.confirming ||
                (n.confirm_done_ms != 0u &&
                 now - n.confirm_done_ms < PRESENT_CONFIRM_HOLD_MS) ||
                (n.present && now - n.last_motion_ms >= PRESENT_PIE_QUIET_MS &&
                 now - n.last_motion_ms < PRESENT_HOLD_MS);
            next_display = now + (timing ? DISPLAY_FAST_MS : DISPLAY_PERIOD_MS);
            push_display(&n, &in);
        }

        if (now >= next_hb) {
            next_hb = now + HB_PERIOD_MS;
            esp_link_send_hb((now - boot_ms) / 1000u, n.present);

            /* Repeated status, not a single boot line: pico_stdio_usb drops
             * everything written before the host asserts DTR, which is always
             * the case for messages logged during boot. One line every 5 s is
             * the difference between a diagnostic and a decoration. */
            const esp_link_in_t *i = &in;
            DIAG("[roomnode] up=%us present=%d sensor=%d func=0x%02X tpres=%d "
                 "tamb=%d servo=%d%% state=%u net=%d epoch=%lld "
                 "sent=%lu fail=%lu\n",
                 (unsigned)((now - boot_ms) / 1000u), n.present ? 1 : 0,
                 n.sensor_ok ? 1 : 0, n.func_status, n.tpresence, n.tamb_raw,
                 servo_percent(), (unsigned)i->state,
                 i->net_valid && i->net_up ? 1 : 0, (long long)i->epoch,
                 (unsigned long)n.frames_sent, (unsigned long)n.frames_fail);

            DIAG("[roomnode] c6rx bytes=%lu lines=%lu bad=%lu long=%lu "
                 "err=0x%02X rxpin=%d status=%lu net=%lu time=%lu other=%lu "
                 "valid=%d/%d\n",
                 (unsigned long)i->rx_bytes, (unsigned long)i->rx_lines,
                 (unsigned long)i->rx_malformed,
                 (unsigned long)i->rx_overlong, i->rx_error_flags,
                 gpio_get(PIN_IO_UART_RX) ? 1 : 0,
                 (unsigned long)i->rx_status, (unsigned long)i->rx_net,
                 (unsigned long)i->rx_time, (unsigned long)i->rx_other,
                 i->status_valid ? 1 : 0, i->net_valid ? 1 : 0);

            /* The whole raw picture on one line. tpresence is what the verdict
             * uses, but tobj/tamb/tobj_comp are what explain a value that
             * drifts: tpresence tracks the ambient with a lag, so a room that
             * is warming or cooling shows up here and nowhere else. */
            DIAG("[roomnode] raw tobj=%d tamb=%d comp=%d tpres=%d base=%d "
                 "d=%d flags=0x%02X quiet=%lums\n",
                 n.tobj_raw, n.tamb_raw, n.tobj_comp, n.tpresence,
                 n.tpres_base, n.tpres_delta, n.func_status,
                 (unsigned long)(n.present
                     ? (to_ms_since_boot(get_absolute_time()) - n.last_motion_ms)
                     : 0u));
        }

        /* Keep UART1 below its 32-byte FIFO depth. At 115200 baud the C6's
         * net/status/time burst can add about 12 bytes per millisecond; the old
         * 20 ms sleep guaranteed an overrun before the next poll. Presence
         * sampling remains independently scheduled at SAMPLE_MS. */
        sleep_ms(1);
    }
}
