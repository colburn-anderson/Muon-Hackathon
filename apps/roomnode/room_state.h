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
#define ROOM_MSG_ACTION    0x52u
#define ROOM_STATE_VERSION 5u

/* The two windows in which the presence verdict is still undecided. main owns
 * both timers -- they are policy, and this CPU is not allowed policy -- so it
 * reports how far through them it is and the display draws the progress. The
 * display therefore never needs to know the window lengths, only the fraction
 * it is handed. */
typedef enum {
    ROOM_PRES_STEADY = 0,   /* nothing pending; draw no progress */
    ROOM_PRES_ENTERING = 1, /* confirmation window: this may become present */
    ROOM_PRES_EXITING = 2,  /* hold window: this may become absent */
} room_pres_phase_t;

typedef enum {
    ROOM_FREE = 0,
    ROOM_HELD = 1,
    ROOM_RESERVED = 2,
    ROOM_WARN = 3,
    ROOM_UNKNOWN = 4,
} room_state_t;

/* FUNC_STATUS bits, named here because the frame carries the raw byte across
 * the link and the display decodes it. */
#define ROOM_FUNC_SHOCK 0x01u   /* TAMB_SHOCK_FLAG */
#define ROOM_FUNC_MOT   0x02u   /* MOT_FLAG */
#define ROOM_FUNC_PRES  0x04u   /* PRES_FLAG */

#define ROOM_ALERT_NONE   0u
#define ROOM_ALERT_WARN   1u
#define ROOM_ALERT_URGENT 2u

typedef enum {
    ROOM_ACTION_END = 1,
    ROOM_ACTION_TEST_SERVO_LOCK = 2,
    ROOM_ACTION_TEST_SERVO_UNLOCK = 3,
    /* Put the door back where the service last put it. The servo tests above
     * overwrite that position and nothing else would ever restore it: the C6
     * only sends a command when the service's value *changes*, so a stale test
     * position could stand indefinitely. Sent when the exerciser is exited. */
    ROOM_ACTION_SERVO_RESTORE = 4,
} room_action_id_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t action;        /* room_action_id_t */
} room_action_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;       /* ROOM_STATE_VERSION */
    uint8_t  state;         /* room_state_t */
    uint8_t  present;       /* sensor says a body is here */
    uint8_t  net_up;        /* C6 could reach the API */
    uint8_t  alert_level;   /* ROOM_ALERT_* */
    uint8_t  sensor_ok;     /* 0 = sensor did not identify itself */
    uint16_t reserved0;
    uint32_t elapsed_s;     /* since presence began, 0 when absent */
    int32_t  hold_left_s;    /* negative = unknown/not applicable */
    int64_t  epoch;          /* current UTC second, 0 until API Date sync */
    /* Raw sensor readings, carried so they can be watched while tuning.
     * tpresence is the part's own presence metric; tamb_raw is ambient. */
    int16_t  tpresence;
    int16_t  tamb_raw;
    int16_t  tpres_delta;   /* excursion above the tracked empty-room floor */
    uint8_t  func_status;   /* PRES_FLAG | MOT_FLAG | TAMB_SHOCK_FLAG */
    uint8_t  pres_phase;    /* room_pres_phase_t */
    uint8_t  pres_percent;  /* 0..100 through that window, 0 when steady */
    char     alert[48];     /* reason text, empty when none */
    char     holder[32];    /* who holds it, empty when nobody */
    char     qr_url[192];   /* service URL; empty draws no QR at all */
} room_frame_t;

#endif
