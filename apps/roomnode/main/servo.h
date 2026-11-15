/* Door-lock servo on breakout GPIO27.
 *
 * Open loop: a hobby servo wants a 1-2 ms pulse every 20 ms and we have no
 * position feedback, so "locked" and "unlocked" are just two angles and the
 * code never claims more than that.
 *
 * Hardware caveat worth knowing before debugging this: breakout GPIO26/27 are
 * FPGA-side pads on the OG (bsp/common/io_cfg.h: "FPGA only -- not on the
 * expander"), so the RP2040 pad only reaches the header if the gateware routes
 * it. If the servo does not twitch while the pulse is provably being generated,
 * that is the first thing to suspect -- not this file. */

#ifndef FWOG_SERVO_H
#define FWOG_SERVO_H

#include <stdint.h>

#define SERVO_PIN_GPIO 27u

/* Pulse widths, in microseconds, for the two positions that matter. */
#define SERVO_US_LOCKED   1000u
#define SERVO_US_OPEN     2000u
#define SERVO_TRAVEL_US   (SERVO_US_OPEN - SERVO_US_LOCKED)

void servo_init(void);

/* 0..100 percent of travel, 0 = locked. Values outside are clamped: a backend
 * bug should not drive the horn into its stop and stall the motor. */
void servo_set_percent(int percent);

int servo_percent(void);

#endif
