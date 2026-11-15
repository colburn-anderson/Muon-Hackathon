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
#define PRESENT_HOLD_MS      30000u   /* how long one motion event counts for */
#define PRESENT_CONFIRM_MS   600u     /* 3 samples before believing a body */

#define DISPLAY_PERIOD_MS    500u
#define HB_PERIOD_MS         5000u
#define IO_APPLY_TRIES       20u
#define IO_APPLY_RETRY_MS    50u

typedef struct {
    bool     present;         /* debounced */
    uint32_t present_since_ms;
    uint32_t last_motion_ms;
    uint32_t confirm_since_ms;
    bool     confirming;
    bool     sensor_ok;

    char     alert[48];
    uint8_t  alert_level;

    int64_t  epoch;           /* wall clock estimate, 0 = unknown */
    uint32_t epoch_at_ms;

    /* Last raw reading, kept only so the status line can show it. A presence
     * algorithm that never fires looks identical to a sensor with no data, and
     * these three numbers are what tells the two apart on the bench. */
    uint8_t  func_status;
    int16_t  tpresence;
    int16_t  tamb_raw;
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

static void push_display(const node_t *n, const esp_link_in_t *in) {
    room_frame_t f;
    memset(&f, 0, sizeof f);
    f.version = ROOM_STATE_VERSION;
    f.state = in->status_valid ? (uint8_t)in->state : (uint8_t)ROOM_UNKNOWN;
    f.present = n->present ? 1u : 0u;
    f.net_up = (in->net_valid && in->net_up) ? 1u : 0u;
    f.sensor_ok = n->sensor_ok ? 1u : 0u;
    f.alert_level = n->alert_level;
    f.tpresence = n->tpresence;
    f.tamb_raw = n->tamb_raw;
    f.func_status = n->func_status;
    snprintf(f.alert, sizeof f.alert, "%s", n->alert);
    snprintf(f.holder, sizeof f.holder, "%s", in->holder);
    snprintf(f.qr_url, sizeof f.qr_url, "%s", ROOM_QR_URL);

    if (n->present) {
        f.elapsed_s = (to_ms_since_boot(get_absolute_time()) - n->present_since_ms) / 1000u;
    }

    /* How long the current hold has left. Negative means "does not apply",
     * which is what the display renders as a dash rather than "0:00". */
    f.hold_left_s = -1;
    const int64_t now = clock_now(n);
    if (now > 0 && in->status_valid && in->expires_at > 0 &&
        (in->state == ROOM_HELD || in->state == ROOM_RESERVED || in->state == ROOM_WARN)) {
        f.hold_left_s = (int32_t)(in->expires_at - now);
    }

    uint8_t payload[1u + sizeof f];
    payload[0] = ROOM_MSG_STATE;
    memcpy(payload + 1, &f, sizeof f);
    (void)fwog_link_uart_send_frame(payload, sizeof payload);
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

        if (in.time_valid) {
            clock_sync(&n, in.epoch);
            in.time_valid = false;
        }

        if (in.servo_valid) {
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
            n.tamb_raw = s.tamb_raw;

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
            next_display = now + DISPLAY_PERIOD_MS;
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
                 "tamb=%d servo=%d%% state=%u net=%d epoch=%lld\n",
                 (unsigned)((now - boot_ms) / 1000u), n.present ? 1 : 0,
                 n.sensor_ok ? 1 : 0, n.func_status, n.tpresence, n.tamb_raw,
                 servo_percent(), (unsigned)i->state,
                 i->net_valid && i->net_up ? 1 : 0, (long long)i->epoch);
        }

        /* The loop is sample-rate limited; sleeping keeps the watchdog kick
         * honest and leaves the USB stack and the link room to breathe. */
        sleep_ms(20);
    }
}
