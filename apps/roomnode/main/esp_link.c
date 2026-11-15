#include "esp_link.h"

#include <stdio.h>
#include <string.h>

#include "fwog_main.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"

#include "common/json/jhelp.h"

static char   s_line[ESP_LINE_MAX];
static size_t s_line_len;
static bool   s_dropping;   /* discarding the tail of an over-long line */

bool esp_link_init(void) {
    /* set_dir() inside fwog_io_dir_apply() ends with gpio_init(), which selects
     * SIO -- so the UART function must be selected after, never before. */
    uart_init(ESP_UART, ESP_UART_BAUD);
    gpio_set_function(PIN_IO_UART_TX,  GPIO_FUNC_UART);
    gpio_set_function(PIN_IO_UART_RX,  GPIO_FUNC_UART);
    gpio_set_function(PIN_IO_UART_CTS, GPIO_FUNC_UART);
    gpio_set_function(PIN_IO_UART_RTS, GPIO_FUNC_UART);

    /* Idle-high inputs. Without these an unplugged C6 leaves RX and CTS
     * floating, which reads as a permanent break / permanent "not clear to
     * send" rather than as an absent peer. */
    gpio_pull_up(PIN_IO_UART_RX);
    gpio_pull_up(PIN_IO_UART_CTS);

    uart_set_hw_flow(ESP_UART, true, true);
    uart_set_format(ESP_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(ESP_UART, true);
    return true;
}

static room_state_t parse_state(const char *s) {
    if (strcmp(s, "free") == 0)     return ROOM_FREE;
    if (strcmp(s, "held") == 0)     return ROOM_HELD;
    if (strcmp(s, "reserved") == 0) return ROOM_RESERVED;
    if (strcmp(s, "warn") == 0)     return ROOM_WARN;
    return ROOM_UNKNOWN;
}

/* One complete line from the C6. Unknown types are ignored: the C6 may add
 * messages this firmware has never heard of. */
static void handle_line(esp_link_in_t *in, char *line) {
    char t[16];
    if (!jh_string(line, "t", t, sizeof t)) {
        return;
    }

    if (strcmp(t, "status") == 0) {
        char state[16] = "";
        if (jh_string(line, "state", state, sizeof state)) {
            in->state = parse_state(state);
            in->status_valid = true;
        }
        if (!jh_string(line, "holder", in->holder, sizeof in->holder)) {
            in->holder[0] = '\0';
        }
        double d = 0;
        in->expires_at = jh_number(line, "expires_at", &d) ? (int64_t)d : 0;
        in->next_res_start = jh_number(line, "next_res_start", &d) ? (int64_t)d : 0;
    } else if (strcmp(t, "net") == 0) {
        bool up = false;
        if (jh_bool(line, "up", &up)) {
            in->net_up = up;
            in->net_valid = true;
        }
    } else if (strcmp(t, "time") == 0) {
        double d = 0;
        if (jh_number(line, "epoch", &d) && d > 0) {
            in->epoch = (int64_t)d;
            in->time_valid = true;
        }
    } else if (strcmp(t, "cmd") == 0) {
        double d = 0;
        if (jh_number(line, "servo", &d)) {
            in->servo_percent = (int)d;
            in->servo_valid = true;
        }
    } else if (strcmp(t, "alert") == 0) {
        double d = 1;
        jh_number(line, "level", &d);
        if (!jh_string(line, "text", in->alert, sizeof in->alert)) {
            in->alert[0] = '\0';
        }
        in->alert_level = in->alert[0] ? (uint8_t)d : ROOM_ALERT_NONE;
        in->alert_valid = true;
    }
    /* "hb" and "boot" are for the log only; the C6 does not send them. */
}

void esp_link_poll(esp_link_in_t *in) {
    /* Bounded per call: the UART FIFO plus a bit. A flooding peer must not
     * starve the presence loop or the watchdog beat. */
    for (int budget = 0; budget < 256; budget++) {
        if (!uart_is_readable(ESP_UART)) {
            break;
        }
        const char c = (char)uart_getc(ESP_UART);

        if (c == '\n') {
            if (!s_dropping && s_line_len > 0u) {
                s_line[s_line_len] = '\0';
                handle_line(in, s_line);
            }
            s_line_len = 0u;
            s_dropping = false;
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (s_line_len + 1u >= sizeof s_line) {
            /* Refuse to grow: mark the line bad and skip to the newline, which
             * turns a corrupt message into a dropped one instead of a partial
             * parse against a truncated buffer. */
            s_dropping = true;
            s_line_len = 0u;
            continue;
        }
        if (!s_dropping) {
            s_line[s_line_len++] = c;
        }
    }
}

static void send(const char *line) {
    uart_puts(ESP_UART, line);
    uart_putc(ESP_UART, '\n');
}

void esp_link_send_presence(bool present) {
    send(present ? "{\"t\":\"presence\",\"v\":true}"
                 : "{\"t\":\"presence\",\"v\":false}");
}

void esp_link_send_hb(uint32_t up_s, bool present) {
    char line[64];
    snprintf(line, sizeof line, "{\"t\":\"hb\",\"up_s\":%lu,\"present\":%s}",
             (unsigned long)up_s, present ? "true" : "false");
    send(line);
}

void esp_link_send_boot(const char *fw_version) {
    char line[64];
    snprintf(line, sizeof line, "{\"t\":\"boot\",\"fw\":\"%s\"}", fw_version);
    send(line);
}
