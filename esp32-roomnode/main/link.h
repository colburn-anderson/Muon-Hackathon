/* JSON-lines link to the main CPU over UART0 (GPIO16/17, 115200 baud).
 *
 * One JSON object per line, '\n' terminated, both directions. Text rather than
 * binary framing keeps the low-rate control traffic directly inspectable with
 * a serial terminal. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef void (*rn_presence_cb_t)(bool present);
typedef void (*rn_link_cb_t)(void);

typedef struct {
    uint32_t tx_lines;
    uint32_t tx_bytes;
    uint32_t tx_short_writes;
    bool     tx_pin_high;
} rn_link_stats_t;

void rn_link_init(void);

/* Called from the link task when the main CPU reports a presence change. */
void rn_link_on_presence(rn_presence_cb_t cb);
void rn_link_on_end(rn_link_cb_t cb);
/* Called when the main CPU announces a boot. Everything main believed died
 * with it, including the servo position, so anything the C6 thinks it has
 * already told main is no longer true. */
void rn_link_on_boot(rn_link_cb_t cb);

/* Outbound. All of these are called from the API task; the UART driver
 * serialises writes internally, and a torn line is not possible below the
 * driver's write queue. */
void rn_link_status(const char *state, const char *holder, const char *url,
                    int64_t expires_at, int64_t next_res_start);
void rn_link_cmd(int servo_angle);
void rn_link_alert(const char *text, int level);
void rn_link_time(int64_t epoch);
void rn_link_net(bool up, const char *api);

/* Snapshot of bytes accepted by the IDF UART driver. */
void rn_link_get_stats(rn_link_stats_t *out);

/* FreeRTOS task entry: drains the UART and dispatches lines. */
void rn_link_task(void *arg);
