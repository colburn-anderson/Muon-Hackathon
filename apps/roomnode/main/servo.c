#include "servo.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

/* 50 Hz: 20 ms period. A 1 MHz tick makes the level register read directly in
 * microseconds, which is why the divider below is derived rather than guessed
 * -- clk_peri is pinned to clk_sys at 200 MHz by the board header, and a
 * hardcoded divider would silently become wrong if that ever changed. */
#define SERVO_HZ        50u
#define SERVO_TICK_HZ   1000000u
#define SERVO_WRAP      (SERVO_TICK_HZ / SERVO_HZ)   /* 20000 us */

static unsigned s_slice;
static int      s_percent = 0;

static uint16_t percent_to_level(int percent) {
    const uint32_t span = SERVO_TRAVEL_US;
    const uint32_t us = SERVO_US_LOCKED + (uint32_t)((span * (uint32_t)percent) / 100u);
    return (uint16_t)us;
}

void servo_init(void) {
    gpio_set_function(SERVO_PIN_GPIO, GPIO_FUNC_PWM);
    s_slice = pwm_gpio_to_slice_num(SERVO_PIN_GPIO);

    /* clk_peri -> 1 MHz. Rounded up: a fractional divider cannot exceed the
     * requested tick rate without shortening the period. */
    const uint32_t clk = clock_get_hz(clk_peri);
    const float divider = (float)clk / (float)SERVO_TICK_HZ;

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, divider);
    pwm_config_set_wrap(&cfg, (uint16_t)(SERVO_WRAP - 1u));
    pwm_init(s_slice, &cfg, true);

    /* Start locked rather than wherever the horn happened to be. */
    servo_set_percent(0);
}

void servo_set_percent(int percent) {
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    s_percent = percent;

    const uint16_t level = percent_to_level(percent);
    pwm_set_gpio_level(SERVO_PIN_GPIO, level);
}

int servo_percent(void) { return s_percent; }
