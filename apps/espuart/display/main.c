/* ESP32 UART monitor -- display-CPU half.
 *
 * Receives 0x40 frames from main (payload: [0x40][raw UART bytes...]) and
 * renders them as a 53x30 character terminal on the LCD. The same bytes are
 * mirrored to this CPU's USB CDC.
 *
 * Rendering is deliberately simple: a character grid, a software scroll when
 * the bottom row is reached, and a full redraw of dirty rows at most every
 * RENDER_MS. At scale 1 a full screen is 30 rows x 53 glyphs, which the
 * panel paints in a few tens of milliseconds -- well under the rate a human
 * can read, and the inter-CPU link's hardware flow control absorbs the
 * difference while a redraw is in progress.
 */
#include "fwog_display.h"
#include "pico/stdlib.h"
#if FWOG_DIAG == FWOG_DIAG_USB
#include "pico/stdio_usb.h"
#endif
#include <stdio.h>
#include <string.h>

FWOG_POWER_DEFAULT();

/* Must match apps/espuart/main/main.c. */
#define ESPUART_MSG_DATA 0x40u

#define ESP_COLS   (ST7789_W / LCD_GLYPH_W)   /* 53 */
#define ESP_ROWS   (ST7789_H / LCD_GLYPH_H)   /* 30 */
#define ESP_FG     0xFFFFu
#define ESP_BG     0x0000u
#define RENDER_MS  100u

static bool          s_link_up;
static fwog_link_rx_t s_rx;

/* The visible character grid. Each row is NUL-terminated at s_len[]; the
 * rest of the row is not meaningful and lcd_text_draw_padded() erases it. */
static char    s_text[ESP_ROWS][ESP_COLS + 1u];
static uint8_t s_len[ESP_ROWS];
static bool    s_dirty[ESP_ROWS];
static bool    s_full_redraw = true;

static unsigned s_row;
static unsigned s_col;
static bool     s_after_cr;
static bool     s_got_data;

static void reset_screen(void) {
    memset(s_text, 0, sizeof s_text);
    memset(s_len, 0, sizeof s_len);
    memset(s_dirty, 0, sizeof s_dirty);
    s_row = 0u;
    s_col = 0u;
    s_after_cr = false;
    s_full_redraw = true;
}

static void draw_row(unsigned row) {
    if (!st7789_ready()) return;
    s_text[row][s_len[row]] = '\0';
    lcd_text_draw_padded(0u, (uint16_t)(row * LCD_GLYPH_H), s_text[row],
                         ESP_COLS, 1u, ESP_FG, ESP_BG);
    s_dirty[row] = false;
}

static void render_dirty(void) {
    if (!st7789_ready()) return;

    if (s_full_redraw) {
        s_full_redraw = false;
        for (unsigned r = 0u; r < ESP_ROWS; r++) draw_row(r);
        return;
    }
    for (unsigned r = 0u; r < ESP_ROWS; r++) {
        if (s_dirty[r]) draw_row(r);
    }
}

static void scroll_up(void) {
    memmove(s_text[0], s_text[1], sizeof s_text[0] * (ESP_ROWS - 1u));
    memmove(s_len, s_len + 1, sizeof s_len[0] * (ESP_ROWS - 1u));
    s_text[ESP_ROWS - 1u][0] = '\0';
    s_len[ESP_ROWS - 1u] = 0u;
    memset(s_dirty, 1, sizeof s_dirty);
}

static void new_line(void) {
    s_col = 0u;
    if (s_row + 1u < ESP_ROWS) {
        s_row++;
        s_len[s_row] = 0u;
        s_text[s_row][0] = '\0';
        s_dirty[s_row] = true;
    } else {
        scroll_up();
    }
}

static void put_char(char c) {
    if (s_col >= ESP_COLS) new_line();
    s_text[s_row][s_col++] = c;
    if (s_col > s_len[s_row]) s_len[s_row] = (uint8_t)s_col;
    s_text[s_row][s_len[s_row]] = '\0';
    s_dirty[s_row] = true;
}

/* One received byte, interpreted as terminal text. Control bytes other than
 * CR/LF/tab/backspace are dropped from the display -- they are still present
 * in the raw USB CDC mirror. */
static void feed_byte(uint8_t b) {
    /* A CR is held for one byte so "\r\n" is one line break, not two, while
     * a lone "\r" (the usual progress-line idiom) returns to column 0 and
     * overwrites the current line. */
    if (s_after_cr) {
        s_after_cr = false;
        if (b == '\n') {
            new_line();
            return;
        }
        s_col = 0u;
        s_len[s_row] = 0u;
        s_text[s_row][0] = '\0';
        s_dirty[s_row] = true;
        if (b == '\r') {
            s_after_cr = true;
            return;
        }
    }

    switch (b) {
    case '\r':
        s_after_cr = true;
        return;
    case '\n':
        new_line();
        return;
    case '\t':
        do {
            put_char(' ');
        } while ((s_col % 8u) != 0u);
        return;
    case '\b':
    case 0x7Fu:
        if (s_col > 0u) {
            s_col--;
            s_text[s_row][s_col] = ' ';
            if (s_len[s_row] > s_col) s_len[s_row] = (uint8_t)s_col;
            s_text[s_row][s_len[s_row]] = '\0';
            s_dirty[s_row] = true;
        }
        return;
    default:
        break;
    }

    if (b < 0x20u) return;          /* other control codes: display nothing */
    if (b > 0x7Eu) b = (uint8_t)'.'; /* keep non-ASCII visible as a dot */
    put_char((char)b);
}

static void mirror_to_usb(const uint8_t *data, size_t len) {
#if FWOG_DIAG == FWOG_DIAG_USB
    if (!stdio_usb_connected()) return;
#endif
    (void)fwrite(data, 1, len, stdout);
    (void)fflush(stdout);
}

static void service_link(void) {
    if (!s_link_up) return;

    uint8_t b;
    while (fwog_link_uart_read(&b)) {
        size_t len = 0u;
        if (!fwog_link_rx_byte(&s_rx, b, &len)) continue;

        /* Main also uses this link for breakout-direction requests; answer
         * those first. 0x40 does not collide with its 0x20-0x22 range. */
        if (fwog_ioexp_link_handle(s_rx.buf, len)) continue;

        if (len < 2u || s_rx.buf[0] != ESPUART_MSG_DATA) continue;

        const uint8_t *data = s_rx.buf + 1;
        const size_t n = len - 1u;

        if (!s_got_data) {
            s_got_data = true;
            reset_screen();
        }
        for (size_t i = 0u; i < n; i++) feed_byte(data[i]);
        mirror_to_usb(data, n);
    }
}

int main(void) {
    board_init();

    /* The display bootloader deinits the link before jumping here, so the
     * application brings it back up -- this is what lets main's
     * fwog_io_dir_apply() complete while the panel is still initializing. */
    s_link_up = fwog_link_uart_init(FWOG_LINK_BAUD);
    fwog_link_rx_init(&s_rx);

    DIAG("\n[espuart_display] alive, link=%s\n",
         s_link_up ? "ok" : "FAILED");

    /* Staged init, with the link serviced between steps: main may already be
     * waiting on the expander ack that unlocks the ESP32 UART pads. */
    st7789_init_begin();
    const absolute_time_t lcd_deadline = make_timeout_time_ms(1000);
    while (!st7789_ready() && !time_reached(lcd_deadline)) {
        service_link();
        (void)fwog_power_poll(to_ms_since_boot(get_absolute_time()));
        st7789_init_step();
        tight_loop_contents();
    }

    if (st7789_ready()) {
        st7789_clear(ESP_BG);
        st7789_dma_wait();
        board_backlight(255);

        reset_screen();
        static const char hint[] =
            "waiting for ESP32 on UART1 8N1 3Mbaud";
        for (size_t i = 0u; i < sizeof hint - 1u; i++) feed_byte((uint8_t)hint[i]);
        render_dirty();
    } else {
        DIAG("[espuart_display] panel never became ready\n");
    }

    absolute_time_t next_render = make_timeout_time_ms(RENDER_MS);
    while (true) {
        service_link();
        (void)fwog_power_poll(to_ms_since_boot(get_absolute_time()));

        if (time_reached(next_render)) {
            next_render = make_timeout_time_ms(RENDER_MS);
            render_dirty();
        }
        tight_loop_contents();
    }
}
