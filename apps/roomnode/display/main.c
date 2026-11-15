/* Room node, display CPU: pixels only.
 *
 * It knows nothing about the network, the sensor or the servo. main sends one
 * room_frame_t per half second and this renders it. Everything on screen is
 * therefore a claim main made, which is the point: one source of truth, and a
 * display that cannot disagree with the door.
 *
 * Contract: FWOG_POWER_DEFAULT() + fwog_power_poll() every iteration (the red
 * 6 s hold is the only way to power this board off), and no watchdog on this
 * CPU, ever.
 *
 * Drawing policy: NO full-screen clear after the first paint. Every region owns
 * its rectangle, paints its own background, and is redrawn only when the value
 * it shows changes. Clearing and repainting the world on each 500 ms frame
 * flickers the whole panel -- visible, and worse than useless on a status
 * display somebody glances at from across the room. */

#include "fwog_display.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#include "room_state.h"

FWOG_POWER_DEFAULT();

#define COL_BG        0x0000u
#define COL_FG        0xFFFFu
#define COL_DIM       0x8410u
#define COL_GREEN     0x07E0u
#define COL_AMBER     0xFD20u
#define COL_RED       0xF800u
#define COL_BLUE      0x2C9Fu

/* Fixed layout, 320x240. Every rectangle below is disjoint from the others. */
#define HDR_Y      4u
#define STATE_Y   32u
#define PRES_Y    72u
#define HOLD_Y   104u
#define HOLD_X   176u    /* right of the state word, left of the QR */
#define WHO_Y    128u
#define BAR_Y    164u
#define BAR_H     40u
#define RAW_Y    208u
#define URL_Y    220u

#define QR_X  176u
#define QR_Y   24u
#define QR_SZ 136u

#define LEFT_COLS 28u    /* characters that fit left of the QR frame in 6x8 */

#define REFRESH_MS 100u

static bool           s_link_up;
static fwog_link_rx_t s_rx;
static bool           s_have_frame;
static uint32_t       s_frames_rx;
static room_frame_t   s_frame;

/* What the panel is currently showing, so a region can be skipped when nothing
 * it displays has changed. */
static room_frame_t s_drawn;
static bool         s_painted_any;

static uint16_t state_colour(room_state_t st) {
    switch (st) {
    case ROOM_FREE:     return COL_GREEN;
    case ROOM_HELD:     return COL_BLUE;
    case ROOM_RESERVED: return COL_AMBER;
    case ROOM_WARN:     return COL_RED;
    default:            return COL_DIM;
    }
}

static const char *state_word(room_state_t st) {
    switch (st) {
    case ROOM_FREE:     return "FREE";
    case ROOM_HELD:     return "IN USE";
    case ROOM_RESERVED: return "RESERVED";
    case ROOM_WARN:     return "ENDING";
    default:            return "UNKNOWN";
    }
}

/* mm:ss, and h:mm:ss once an hour has passed -- a room held all afternoon
 * should not read 123:45. */
static void format_duration(uint32_t seconds, char *out, size_t outsz) {
    if (seconds >= 3600u) {
        snprintf(out, outsz, "%u:%02u:%02u", (unsigned)(seconds / 3600u),
                 (unsigned)((seconds / 60u) % 60u), (unsigned)(seconds % 60u));
    } else {
        snprintf(out, outsz, "%02u:%02u", (unsigned)(seconds / 60u),
                 (unsigned)(seconds % 60u));
    }
}

static void draw_wrapped(uint16_t x, uint16_t y, const char *text, unsigned cols,
                         unsigned rows, unsigned scale, uint16_t fg, uint16_t bg) {
    char line[64];
    const size_t n = strlen(text);
    unsigned row = 0;
    for (size_t i = 0; i < n && row < rows; i += cols, row++) {
        const size_t take = (n - i < cols) ? (n - i) : cols;
        memcpy(line, text + i, take);
        line[take] = '\0';
        lcd_text_draw_padded(x, (uint16_t)(y + row * LCD_GLYPH_H * scale), line,
                             cols, scale, fg, bg);
    }
}

/* ---- regions ---------------------------------------------------------- */

static void paint_header(const room_frame_t *f) {
    lcd_text_draw(8u, HDR_Y, "ROOM 1", 2u, COL_FG, COL_BG);

    lcd_text_draw_padded(120u, HDR_Y + 4u, f->net_up ? "online" : "offline",
                         8u, 1u, f->net_up ? COL_GREEN : COL_RED, COL_BG);
    lcd_text_draw_padded(120u, HDR_Y + 14u, f->sensor_ok ? "sensor ok" : "no sensor",
                         12u, 1u, f->sensor_ok ? COL_DIM : COL_AMBER, COL_BG);
}

static void paint_state(const room_frame_t *f) {
    const room_state_t st = (room_state_t)f->state;
    lcd_text_draw_padded(8u, STATE_Y, state_word(st), 9u, 3u, state_colour(st), COL_BG);
}

static void paint_presence(const room_frame_t *f) {
    lcd_text_draw_padded(8u, PRES_Y, f->present ? "present" : "nobody", 8u, 1u,
                         COL_DIM, COL_BG);
    char dur[16];
    format_duration(f->elapsed_s, dur, sizeof dur);
    lcd_text_draw_padded(8u, PRES_Y + 10u, dur, 12u, 2u, f->present ? COL_FG : COL_DIM, COL_BG);
}

static void paint_hold(const room_frame_t *f) {
    /* Time left on the hold, when the backend told us. Drawn right of the state
     * word so it cannot collide with the holder line below. */
    if (f->hold_left_s >= 0) {
        char left[16];
        format_duration((uint32_t)f->hold_left_s, left, sizeof left);
        lcd_text_draw_padded(HOLD_X - 44u, HOLD_Y, "left", 5u, 1u, COL_DIM, COL_BG);
        lcd_text_draw_padded(HOLD_X - 44u, HOLD_Y + 8u, left, 10u, 2u,
                             state_colour((room_state_t)f->state), COL_BG);
    } else {
        lcd_text_draw_padded(HOLD_X - 44u, HOLD_Y, "", 5u, 1u, COL_DIM, COL_BG);
        lcd_text_draw_padded(HOLD_X - 44u, HOLD_Y + 8u, "", 10u, 2u, COL_FG, COL_BG);
    }
}

static void paint_holder(const room_frame_t *f) {
    draw_wrapped(8u, WHO_Y, f->holder, LEFT_COLS, 2u, 1u, COL_DIM, COL_BG);
    /* Clear the second line when the first one is short, since draw_wrapped
     * only pads the rows it actually draws. */
    if (strlen(f->holder) <= LEFT_COLS) {
        lcd_text_draw_padded(8u, WHO_Y + LCD_GLYPH_H, "", LEFT_COLS, 1u, COL_DIM, COL_BG);
    }
}

static void paint_alert(const room_frame_t *f) {
    if (f->alert_level != ROOM_ALERT_NONE && f->alert[0] != '\0') {
        const uint16_t bar = (f->alert_level >= ROOM_ALERT_URGENT) ? COL_RED : COL_AMBER;
        st7789_fill_rect(0u, BAR_Y, ST7789_W, BAR_H, bar);
        draw_wrapped(8u, BAR_Y + 6u, f->alert, 26u, 2u, 2u, COL_BG, bar);
    } else {
        st7789_fill_rect(0u, BAR_Y, ST7789_W, BAR_H, COL_BG);
    }
}

/* The tuning line. tpresence is the sensor's own presence metric, tamb the
 * ambient raw, func the flag byte -- PRES_FLAG(0x04) and MOT_FLAG(0x02). These
 * are what decide the threshold, so they belong where a person with a hand can
 * see them change. */
static void paint_raw(const room_frame_t *f) {
    char line[48];
    snprintf(line, sizeof line, "tpres %d  tamb %d  f 0x%02X",
             f->tpresence, f->tamb_raw, f->func_status);
    lcd_text_draw_padded(8u, RAW_Y, line, 36u, 1u, COL_DIM, COL_BG);
}

static void paint_url(const room_frame_t *f) {
    lcd_text_draw_padded(8u, URL_Y, f->qr_url, 53u, 1u, COL_BLUE, COL_BG);
}

/* Static: painted once, and only again if the URL changes. */
static void paint_qr_frame(void) {
    st7789_fill_rect(QR_X - 4u, QR_Y - 4u, QR_SZ + 8u, QR_SZ + 8u, COL_WHITE);
    st7789_fill_rect(QR_X, QR_Y, QR_SZ, QR_SZ, COL_BG);
    lcd_text_draw_padded(QR_X + 16u, QR_Y + QR_SZ / 2u - 4u, "qr soon", 7u, 2u,
                         COL_DIM, COL_BG);
}

/* Applies F to the panel, painting only the regions whose content changed. */
static void apply_frame(const room_frame_t *f) {
    const room_frame_t *d = &s_drawn;
    const bool full = !s_painted_any;

    if (full) {
        st7789_clear(COL_BG);
        paint_qr_frame();
        paint_url(f);
    }

    if (full || f->net_up != d->net_up || f->sensor_ok != d->sensor_ok) {
        paint_header(f);
    }
    if (full || f->state != d->state) {
        paint_state(f);
    }
    if (full || f->present != d->present || f->elapsed_s != d->elapsed_s) {
        paint_presence(f);
    }
    if (full || f->hold_left_s != d->hold_left_s || f->state != d->state) {
        paint_hold(f);
    }
    if (full || strcmp(f->holder, d->holder) != 0) {
        paint_holder(f);
    }
    if (full || f->alert_level != d->alert_level || strcmp(f->alert, d->alert) != 0) {
        paint_alert(f);
    }
    if (full || f->tpresence != d->tpresence || f->tamb_raw != d->tamb_raw ||
        f->func_status != d->func_status) {
        paint_raw(f);
    }
    if (!full && strcmp(f->qr_url, d->qr_url) != 0) {
        paint_url(f);
    }

    st7789_dma_wait();
    s_drawn = *f;
    s_painted_any = true;
}

static void service_link(void) {
    if (!s_link_up) {
        return;
    }

    uint8_t b;
    while (fwog_link_uart_read(&b)) {
        size_t len = 0u;
        if (!fwog_link_rx_byte(&s_rx, b, &len)) {
            continue;
        }

        /* Main's breakout-direction requests come over this same link and must
         * be answered first, or main never gets its pads configured. */
        if (fwog_ioexp_link_handle(s_rx.buf, len)) {
            continue;
        }

        if (len != 1u + sizeof(room_frame_t) || s_rx.buf[0] != ROOM_MSG_STATE) {
            continue;
        }

        room_frame_t f;
        memcpy(&f, s_rx.buf + 1, sizeof f);

        /* A frame from a mismatched build is refused rather than rendered:
         * wrong firmware must look like wrong firmware. */
        if (f.version != ROOM_STATE_VERSION) {
            DIAG("[roomnode_display] frame version %u, expected %u\n",
                 f.version, ROOM_STATE_VERSION);
            continue;
        }

        f.alert[sizeof f.alert - 1u] = '\0';
        f.holder[sizeof f.holder - 1u] = '\0';
        f.qr_url[sizeof f.qr_url - 1u] = '\0';
        s_frame = f;
        s_have_frame = true;
        s_frames_rx++;
    }
}

int main(void) {
    board_init();

    /* The display bootloader deinits the link before jumping here, so the
     * application brings it back up -- this is what lets main's
     * fwog_io_dir_apply() complete while the panel is still initializing. */
    s_link_up = fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_rx);

    DIAG("\n[roomnode_display] alive, link=%s\n", s_link_up ? "ok" : "FAILED");

    /* Staged init with the link serviced between steps: main may already be
     * waiting on the expander ack that unlocks its UART pads. */
    st7789_init_begin();
    const absolute_time_t lcd_deadline = make_timeout_time_ms(1000);
    while (!st7789_ready() && !time_reached(lcd_deadline)) {
        service_link();
        (void)fwog_power_poll(to_ms_since_boot(get_absolute_time()));
        st7789_init_step();
        tight_loop_contents();
    }

    if (!st7789_ready()) {
        DIAG("[roomnode_display] panel never became ready\n");
    } else {
        st7789_clear(COL_BG);
        st7789_dma_wait();
        board_backlight(255);

        /* Something on screen from the first frame: a black panel is
         * indistinguishable from a hung one. */
        lcd_text_draw(8u, HDR_Y, "ROOM 1", 2u, COL_FG, COL_BG);
        lcd_text_draw_padded(8u, STATE_Y, "waiting", 9u, 3u, COL_DIM, COL_BG);
        lcd_text_draw_padded(8u, PRES_Y, "for main CPU", 16u, 1u, COL_DIM, COL_BG);
    }

    absolute_time_t next_check = make_timeout_time_ms(REFRESH_MS);
    absolute_time_t next_log = make_timeout_time_ms(5000);
    while (true) {
        service_link();
        (void)fwog_power_poll(to_ms_since_boot(get_absolute_time()));

        if (time_reached(next_check)) {
            next_check = make_timeout_time_ms(REFRESH_MS);
            if (s_have_frame) {
                apply_frame(&s_frame);
            }
        }

        /* Repeated, not once at boot: pico_stdio_usb drops anything written
         * before the host asserts DTR, so a boot-time summary is unreadable in
         * practice. This says whether frames are arriving and what was drawn. */
        if (time_reached(next_log)) {
            next_log = make_timeout_time_ms(5000);
            DIAG("[roomnode_display] frames=%lu have=%d state=%u present=%d "
                 "net=%d alert=%u tpres=%d\n",
                 (unsigned long)s_frames_rx, s_have_frame ? 1 : 0,
                 (unsigned)s_frame.state, s_frame.present ? 1 : 0,
                 s_frame.net_up ? 1 : 0, (unsigned)s_frame.alert_level,
                 s_frame.tpresence);
        }
        tight_loop_contents();
    }
}
