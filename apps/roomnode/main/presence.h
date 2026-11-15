/* STHS34PF80 presence sensor on breakout I2C0 (SDA GPIO16 / SCL GPIO17).
 *
 * Register addresses are from the STHS34PF80 register map (datasheet table 15)
 * and cross-checked against two independent drivers, not guessed. The first
 * version of this file guessed 0x20/0x22 for ambient/object, which are CTRL1
 * and CTRL3 -- and it never set an ODR, so the part stayed in power-down and
 * every reading was 0. Both are fixed here.
 *
 * Presence comes from the part's own detection algorithm: FUNC_STATUS (0x25)
 * reports PRES_FLAG (bit 2) and MOT_FLAG (bit 1) with the hysteresis built into
 * the sensor. Reading a flag costs one byte and is what the silicon is for,
 * versus thresholding raw temperatures ourselves and inheriting every drift
 * problem that the embedded algorithm exists to solve.
 *
 * WHO_AM_I mismatch or a NAK mid-run sets ok = false, main reports
 * sensor_ok = 0, and the display says so. The alternative -- claiming a room is
 * empty on no evidence -- is the one failure mode worth burning pixels to
 * avoid. */

#ifndef FWOG_PRESENCE_H
#define FWOG_PRESENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/board.h"

/* Confirmed with a bus scan: the part answers at 0x5A. */
#define PRESENCE_I2C_ADDR 0x5A

/* Breakout I2C0, the user header, not a system bus. board_init() deliberately
 * leaves it un-muxed and un-pulled-up, so this driver does both. */
#define PRESENCE_SDA_PIN PIN_IO_I2C_SDA
#define PRESENCE_SCL_PIN PIN_IO_I2C_SCL

/* Presence verdict: tpresence ABOVE ITS BASELINE by this much is somebody in the
 * room.
 *
 * Why a delta and not an absolute value: measured on this desk, an empty chair
 * read about -2700..-6300 one minute and pointing at the ceiling read +3000 the
 * next. The metric is not a temperature -- it is a filtered, uncalibrated
 * low-frequency IR signal, and its offset moves with what is in the field of
 * view, how the board is angled, self-heating and ambient drift. Any single
 * absolute threshold is therefore wrong within minutes, which is exactly what
 * the first version of this file got wrong.
 *
 * So the driver tracks the low envelope (the empty-room floor) continuously and
 * the verdict is the excursion above it. Fast down, very slow up: the baseline
 * must follow the room emptying within seconds, but must not quietly absorb a
 * person who is sitting still and then declare the room free. */
#ifndef PRESENCE_TPRES_DELTA
#define PRESENCE_TPRES_DELTA 1500
#endif

typedef struct {
    bool    ok;          /* sensor identified itself and answered */
    bool    motion;      /* raw per-frame verdict, before debouncing */
    uint8_t func_status; /* FUNC_STATUS: PRES_FLAG | MOT_FLAG | TAMB_SHOCK_FLAG */
    int16_t tpresence;   /* the presence algorithm's own measured value */
    int16_t tpres_delta; /* tpresence minus the tracked empty-room baseline */
    int16_t tpres_base;  /* the tracked baseline itself, for the log */
    int16_t tobj_raw;    /* object temperature, raw */
    int16_t tobj_comp;   /* ambient-compensated object temperature */
    int16_t tamb_raw;    /* ambient, raw */
} presence_sample_t;

/* Brings up I2C0 and checks the part is there. Returns false (and keeps
 * returning samples with ok = false) when it is not. */
bool presence_init(void);

/* One read. Never blocks longer than a few hundred microseconds when ok. */
presence_sample_t presence_poll(void);

#endif
