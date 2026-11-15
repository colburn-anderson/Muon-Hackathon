#include "presence.h"

#include "fwog_main.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

/* ---- Register map -----------------------------------------------------
 * STHS34PF80 datasheet table 15. The two that matter most are CTRL1 (ODR: the
 * part boots in power-down and reads 0 forever until an ODR is set) and
 * FUNC_STATUS (the presence and motion flags). */

#define REG_WHO_AM_I     0x0Fu
#define WHO_AM_I_VAL     0xD3u

#define REG_LPF1         0x0Cu
#define REG_LPF2         0x0Du
#define REG_AVG_TRIM     0x10u
#define REG_CTRL1        0x20u      /* ODR[3:0] */
#define REG_CTRL2        0x21u      /* FUNC_CFG_ACCESS lives here: never touch */
#define REG_CTRL3        0x22u
#define REG_STATUS       0x23u
#define REG_FUNC_STATUS  0x25u
#define REG_TOBJECT_L    0x26u
#define REG_TAMBIENT_L   0x28u
#define REG_TOBJ_COMP_L  0x38u
#define REG_TPRESENCE_L  0x3Au
#define REG_TMOTION_L    0x3Cu

#define FUNC_PRES_FLAG  0x04u
#define FUNC_MOT_FLAG   0x02u

/* ODR[3:0] encoding, datasheet table 16. 2 Hz: a person walking in produces
 * several samples before our 30 s absence hold could ever expire, and the
 * current draw stays trivial. */
#define ODR_2HZ         0x04u

#define PRESENCE_I2C   i2c0
#define PRESENCE_HZ    400000u

static bool s_ok;

static bool write_u8(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_write_blocking(PRESENCE_I2C, PRESENCE_I2C_ADDR, buf, 2, false) == 2;
}

static bool read_u8(uint8_t reg, uint8_t *out) {
    return i2c_write_blocking(PRESENCE_I2C, PRESENCE_I2C_ADDR, &reg, 1, true) == 1 &&
           i2c_read_blocking(PRESENCE_I2C, PRESENCE_I2C_ADDR, out, 1, false) == 1;
}

/* A 16-bit output is two 8-bit registers latched separately. Without BDU (which
 * this driver does not set, because a wrong bit in CTRL1 is worse than the
 * problem) a conversion landing between the two reads can tear the value. Two
 * agreeing reads in a row is cheap insurance at 400 kHz. */
static bool read_s16_stable(uint8_t reg, int16_t *out) {
    int16_t prev = 0;
    bool have_prev = false;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t b[2];
        if (i2c_write_blocking(PRESENCE_I2C, PRESENCE_I2C_ADDR, &reg, 1, true) != 1) {
            return false;
        }
        if (i2c_read_blocking(PRESENCE_I2C, PRESENCE_I2C_ADDR, b, 2, false) != 2) {
            return false;
        }
        const int16_t v = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
        if (have_prev && v == prev) {
            *out = v;
            return true;
        }
        prev = v;
        have_prev = true;
    }

    /* Three different values: the device is converting faster than we read or
     * something is wrong. Report the last one rather than failing a poll --
     * presence is a flag read and does not depend on this. */
    *out = prev;
    return true;
}

bool presence_init(void) {
    i2c_init(PRESENCE_I2C, PRESENCE_HZ);

    /* The pads must be muxed after the expander apply, which ends in
     * gpio_init() and would otherwise leave them on SIO. */
    gpio_set_function(PRESENCE_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PRESENCE_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PRESENCE_SDA_PIN);
    gpio_pull_up(PRESENCE_SCL_PIN);

    uint8_t who = 0;
    s_ok = read_u8(REG_WHO_AM_I, &who) && who == WHO_AM_I_VAL;
    DIAG("[roomnode] sensor 0x%02X WHO_AM_I=0x%02X %s\n",
         PRESENCE_I2C_ADDR, who, s_ok ? "ok" : "(expected 0xD3) MISMATCH");
    if (!s_ok) {
        return false;
    }

    /* Start converting. Read-modify-write so nothing else in CTRL1 is disturbed
     * -- on a part this small, a stray bit is a stray behaviour. */
    uint8_t ctrl1 = 0;
    if (!read_u8(REG_CTRL1, &ctrl1)) {
        s_ok = false;
        return false;
    }
    const uint8_t want = (uint8_t)((ctrl1 & 0xF0u) | ODR_2HZ);
    if (want != ctrl1 && !write_u8(REG_CTRL1, want)) {
        s_ok = false;
        return false;
    }

    uint8_t ctrl1_now = 0, avg = 0;
    const bool odr_ok = read_u8(REG_CTRL1, &ctrl1_now) && (ctrl1_now & 0x0Fu) == ODR_2HZ;
    (void)read_u8(REG_AVG_TRIM, &avg);
    DIAG("[roomnode] sensor CTRL1 0x%02X -> 0x%02X (odr set: %s), AVG_TRIM 0x%02X\n",
         ctrl1, ctrl1_now, odr_ok ? "yes" : "NO", avg);

    if (!odr_ok) {
        /* Without an ODR there is no data, and pretending otherwise would put a
         * frozen zero on the display forever. */
        s_ok = false;
    }
    return s_ok;
}

presence_sample_t presence_poll(void) {
    presence_sample_t s = {0};

    if (!s_ok) {
        return s;
    }

    if (!read_u8(REG_FUNC_STATUS, &s.func_status)) {
        /* A part that identified itself and then stopped answering has been
         * unplugged or reset: drop ok so the display says so instead of
         * freezing on the last reading forever. */
        s_ok = false;
        DIAG("[roomnode] sensor stopped answering\n");
        return s;
    }

    /* The status read also clears DRDY, which is the documented way to release
     * the output registers for the next conversion. */
    s.motion = (s.func_status & (FUNC_PRES_FLAG | FUNC_MOT_FLAG)) != 0;
    s.ok = true;

    (void)read_s16_stable(REG_TPRESENCE_L, &s.tpresence);
    (void)read_s16_stable(REG_TOBJ_COMP_L, &s.tobj_comp);
    (void)read_s16_stable(REG_TAMBIENT_L, &s.tamb_raw);
    return s;
}
