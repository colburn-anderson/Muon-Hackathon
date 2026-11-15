/* The one struct the two CPUs disagree about nothing on, plus the message id
 * it travels under.
 *
 * Packed and fixed-width on purpose: the display validates `version` and the
 * payload length before trusting a byte of it, because a mismatch between two
 * separately-flashed halves must show as "wrong firmware", never as garbage on
 * the panel. */

#ifndef FWOG_ROOM_STATE_H
#define FWOG_ROOM_STATE_H

#include <stdint.h>

#define ROOM_MSG_STATE     0x51u
#define ROOM_STATE_VERSION 2u

typedef enum {
    ROOM_FREE = 0,
    ROOM_HELD = 1,
    ROOM_RESERVED = 2,
    ROOM_WARN = 3,
    ROOM_UNKNOWN = 4,
} room_state_t;

#define ROOM_ALERT_NONE   0u
#define ROOM_ALERT_WARN   1u
#define ROOM_ALERT_URGENT 2u

typedef struct __attribute__((packed)) {
    uint8_t  version;       /* ROOM_STATE_VERSION */
    uint8_t  state;         /* room_state_t */
    uint8_t  present;       /* sensor says a body is here */
    uint8_t  net_up;        /* C6 could reach the API */
    uint8_t  alert_level;   /* ROOM_ALERT_* */
    uint8_t  sensor_ok;     /* 0 = sensor did not identify itself */
    uint16_t reserved0;
    uint32_t elapsed_s;     /* since presence began, 0 when absent */
    int32_t  hold_left_s;   /* negative = unknown/not applicable */
    /* Raw sensor readings, carried so they can be watched while tuning.
     * tpresence is the part's own presence metric; tamb_raw is ambient. */
    int16_t  tpresence;
    int16_t  tamb_raw;
    uint8_t  func_status;   /* PRES_FLAG | MOT_FLAG | TAMB_SHOCK_FLAG */
    uint8_t  pad0;
    char     alert[48];     /* reason text, empty when none */
    char     holder[32];    /* who holds it, empty when nobody */
    char     qr_url[96];    /* reservation link for the QR */
} room_frame_t;

#endif
