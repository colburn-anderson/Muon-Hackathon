/* Link to the ESP32-C6 on breakout UART1 (GPIO 8 TX / 9 RX / 10 CTS / 11 RTS),
 * 3 Mbaud, JSON lines both ways.
 *
 * Hardware flow control is on from this side: the main CPU pulls the C6's CTS
 * up and drives its RTS. The C6 side deliberately runs without hardware flow
 * control, so if these two ever disagree the symptom is a link that works until
 * the C6's 512-byte RX buffer is full -- not a crash, just a stall under load.
 *
 * Every message is one line of JSON terminated by '\n'. The reader tolerates
 * unknown keys and unknown `t` values on purpose: the C6 is allowed to grow
 * fields without this firmware being reflashed. */

#ifndef FWOG_ESP_LINK_H
#define FWOG_ESP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "room_state.h"

#define ESP_UART       uart1
#define ESP_UART_BAUD  3000000u
#define ESP_LINE_MAX   192u

typedef struct {
    /* Latest published room status. `status_valid` stays false until the C6
     * has answered the API at least once -- the display must say "unknown",
     * not "free", before then. */
    bool        status_valid;
    room_state_t state;
    char        holder[48];
    int64_t     expires_at;
    int64_t     next_res_start;

    bool        net_up;
    bool        net_valid;

    /* Wall clock from the API's Date header, epoch seconds. 0 = never synced. */
    int64_t     epoch;
    bool        time_valid;

    /* Commands, each cleared by the caller after acting on it. */
    bool        servo_valid;
    int         servo_percent;

    bool        alert_valid;
    char        alert[48];
    uint8_t     alert_level;   /* ROOM_ALERT_* */
} esp_link_in_t;

/* Starts UART1 and selects the pin functions. The expander direction word is
 * applied once for the whole app by main(), because the display, the I2C bus
 * and this UART all live in one cfg: two applies would clobber each other and
 * the last one would win. Call after that apply. */
bool esp_link_init(void);

/* Drains whatever has arrived and folds it into IN. Cheap; call every loop. */
void esp_link_poll(esp_link_in_t *in);

void esp_link_send_presence(bool present);
void esp_link_send_hb(uint32_t up_s, bool present);
void esp_link_send_boot(const char *fw_version);

#endif
