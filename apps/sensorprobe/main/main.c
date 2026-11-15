/* I2C0 bring-up probe for the STHS34PF80 presence sensor.
 *
 * Purpose: prove the wiring on breakout I2C0 (SDA GPIO16 / SCL GPIO17) and the
 * part's address before any driver is written. It scans the bus, prints every
 * address that answers, and if the STHS34PF80's usual 0x5A shows up it reads
 * WHO_AM_I (register 0x0F, expected 0xD3) and dumps the low register block so
 * the identity is not a guess.
 *
 * Main CPU only: the sensor hangs off the main CPU's breakout I2C, not the
 * ESP32 and not the display CPU. */

#include "fwog_main.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

FWOG_WATCHDOG_DEFAULT();

#define PROBE_I2C      i2c0
#define PROBE_SDA_PIN  16      /* breakout I2C0 SDA, per docs/hardware/pinmap.md */
#define PROBE_SCL_PIN  17      /* breakout I2C0 SCL */
#define PROBE_BAUD     100000  /* the part takes 400 kHz; 100 kHz survives bad wiring */

#define STHS34PF80_ADDR     0x5A
#define STHS34PF80_WHO_AM_I 0x0F
#define STHS34PF80_WHO_VAL  0xD3

static bool read_reg(uint8_t addr, uint8_t reg, uint8_t *out) {
    if (i2c_write_blocking(PROBE_I2C, addr, &reg, 1, true) != 1) {
        return false;
    }
    return i2c_read_blocking(PROBE_I2C, addr, out, 1, false) == 1;
}

static int scan_bus(uint8_t *found, int max) {
    int n = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t byte = 0;
        /* A zero-length read is the portable "is anyone there?" probe: an ACK
           means a device, a NAK is returned as a negative count. */
        if (i2c_read_blocking(PROBE_I2C, addr, &byte, 1, false) >= 0) {
            if (n < max) {
                found[n++] = addr;
            }
        }
    }
    return n;
}

int main(void) {
    board_init();

    i2c_init(PROBE_I2C, PROBE_BAUD);
    gpio_set_function(PROBE_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PROBE_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PROBE_SDA_PIN);
    gpio_pull_up(PROBE_SCL_PIN);
    sleep_ms(50);   /* let the bus and the sensor settle after power-up */

    /* Repeated rather than printed once: pico_stdio_usb discards anything
       written before the host asserts DTR, which is later than board_init(). */
    const absolute_time_t announce_until = make_timeout_time_ms(8000);
    bool first = true;

    while (true) {
        board_watchdog_kick();

        if (first || time_reached(announce_until)) {
            if (first) {
                DIAG("[sensorprobe] I2C0 on SDA %d / SCL %d at %d Hz\n",
                     PROBE_SDA_PIN, PROBE_SCL_PIN, PROBE_BAUD);
            }

            uint8_t found[16];
            int n = scan_bus(found, 16);
            DIAG("[sensorprobe] scan: %d device(s)\n", n);
            for (int i = 0; i < n; i++) {
                DIAG("[sensorprobe]   0x%02X\n", found[i]);
            }

            uint8_t who = 0;
            if (read_reg(STHS34PF80_ADDR, STHS34PF80_WHO_AM_I, &who)) {
                DIAG("[sensorprobe] 0x%02X WHO_AM_I = 0x%02X (%s)\n",
                     STHS34PF80_ADDR, who,
                     who == STHS34PF80_WHO_VAL ? "STHS34PF80" : "NOT the expected part");
                if (who == STHS34PF80_WHO_VAL) {
                    DIAG("[sensorprobe] regs 0x00..0x1F:");
                    for (uint8_t reg = 0x00; reg <= 0x1F; reg++) {
                        uint8_t v = 0;
                        if (read_reg(STHS34PF80_ADDR, reg, &v)) {
                            DIAG(" %02X", v);
                        } else {
                            DIAG(" --");
                        }
                    }
                    DIAG("\n");
                }
            } else {
                DIAG("[sensorprobe] 0x%02X did not answer\n", STHS34PF80_ADDR);
            }
            first = false;
        }

        sleep_ms(1000);
    }
}
