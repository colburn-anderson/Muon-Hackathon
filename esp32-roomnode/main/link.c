#include "link.h"
#include "roomnode_config.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "jhelp.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "link";

#define LINK_RX_BUF   1024
#define LINK_LINE_MAX 256

static rn_presence_cb_t s_presence_cb;

/* Escapes the few characters our own text can carry into an alert string.
 * The backend's reason strings are the only untrusted-ish input that reaches
 * this path, and a raw quote would break the line for the other CPU. */
static void json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 6 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void send_line(const char *fmt, ...) {
    char line[LINK_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n > sizeof line - 2) {
        n = (int)(sizeof line - 2);
    }
    line[n++] = '\n';
    uart_write_bytes(LINK_UART_NUM, line, (size_t)n);
}

void rn_link_init(void) {
    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,   /* see roomnode_config.h */
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, LINK_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* The main CPU holds its RX line high; a floating line here would turn
     * into a stream of break characters and malformed lines. */
    gpio_pullup_en(LINK_RX_GPIO);

    ESP_LOGI(TAG, "link up: uart%d tx=%d rx=%d %d baud",
             LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO, LINK_BAUD);
}

void rn_link_on_presence(rn_presence_cb_t cb) { s_presence_cb = cb; }

void rn_link_status(const char *state, const char *holder,
                    int64_t expires_at, int64_t next_res_start) {
    char esc_holder[64] = "";
    if (holder != NULL) {
        json_escape(holder, esc_holder, sizeof esc_holder);
    }
    send_line("{\"t\":\"status\",\"state\":\"%s\",\"holder\":\"%s\","
              "\"expires_at\":%lld,\"next_res_start\":%lld}",
              state != NULL ? state : "unknown", esc_holder,
              (long long)expires_at, (long long)next_res_start);
}

void rn_link_cmd(int servo_angle) {
    send_line("{\"t\":\"cmd\",\"servo\":%d}", servo_angle);
}

void rn_link_alert(const char *text, int level) {
    char esc[128] = "";
    if (text != NULL) {
        json_escape(text, esc, sizeof esc);
    }
    send_line("{\"t\":\"alert\",\"text\":\"%s\",\"level\":%d}", esc, level);
}

void rn_link_time(int64_t epoch) {
    if (epoch > 0) {
        send_line("{\"t\":\"time\",\"epoch\":%lld}", (long long)epoch);
    }
}

void rn_link_net(bool up, const char *api) {
    send_line("{\"t\":\"net\",\"up\":%s,\"api\":\"%s\"}",
              up ? "true" : "false", api != NULL ? api : "unknown");
}

/* Handles one complete line from the main CPU. Unknown types are ignored --
 * the main CPU is allowed to grow new messages without bricking this side. */
static void handle_line(char *line) {
    char type[16];
    if (!jh_string(line, "t", type, sizeof type)) {
        ESP_LOGW(TAG, "dropping malformed line: %.60s", line);
        return;
    }

    if (strcmp(type, "presence") == 0) {
        bool v = false;
        if (jh_bool(line, "v", &v) && s_presence_cb != NULL) {
            s_presence_cb(v);
        }
    } else if (strcmp(type, "hb") == 0) {
        double up = -1;
        bool present = false;
        jh_number(line, "up_s", &up);
        jh_bool(line, "present", &present);
        ESP_LOGI(TAG, "main hb: up %ds present %d", (int)up, present ? 1 : 0);
    } else if (strcmp(type, "boot") == 0) {
        char fw[32] = "?";
        jh_string(line, "fw", fw, sizeof fw);
        ESP_LOGI(TAG, "main CPU booted: %s", fw);
    } else {
        ESP_LOGD(TAG, "ignoring type %s", type);
    }
}

void rn_link_task(void *arg) {
    (void)arg;
    char line[LINK_LINE_MAX];
    size_t len = 0;
    uint8_t byte;

    while (true) {
        int n = uart_read_bytes(LINK_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (n != 1) {
            continue;
        }
        if (byte == '\n') {
            line[len] = '\0';
            if (len > 0) {
                handle_line(line);
            }
            len = 0;
        } else if (byte != '\r') {
            if (len < sizeof line - 1) {
                line[len++] = (char)byte;
            } else {
                /* Overlong line: drop it rather than emit a truncated object
                 * that parses as something else. */
                ESP_LOGW(TAG, "line too long, dropped");
                len = 0;
            }
        }
    }
}
