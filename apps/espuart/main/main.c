/* ESP32 UART bridge -- main-CPU half.
 *
 * The ESP32 is wired to the main CPU's breakout UART (UART1):
 *
 *     GPIO 8  UART1 TX   -> ESP32 RX
 *     GPIO 9  UART1 RX   <- ESP32 TX
 *     GPIO 10 UART1 CTS  <- ESP32 RTS   (hardware flow control)
 *     GPIO 11 UART1 RTS  -> ESP32 CTS   (hardware flow control)
 *
 * Those four pads do not go straight to the header: they pass through the
 * FPGA's io_buffer and the display CPU's PCAL6416 level shifter. All three
 * surfaces (FPGA, expander, RP2040 pads) have to agree before the UART can
 * work, which is what fwog_io_dir_apply() does. It needs the display app to
 * be alive and servicing the link, so it runs after
 * fwog_display_update_run() has handed off.
 *
 * Data path:
 *
 *     ESP32 -> UART1 -> 0x40 link frame -> display CPU -> LCD
 *                                      \-> this CPU's USB CDC
 *
 * The frame payload is [0x40][raw bytes...]. 0x40 is above the reserved
 * bootloader range (0x01-0x1F) and above the breakout I/O protocol
 * (0x20-0x22), so the display can tell it apart from io-expander traffic.
 */
#include "fwog_main.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#if FWOG_DIAG == FWOG_DIAG_USB
#include "pico/stdio_usb.h"
#endif
#include <stdio.h>
#include <string.h>

FWOG_WATCHDOG_DEFAULT();

/* Must match apps/espuart/display/main.c. */
#define ESPUART_MSG_DATA 0x40u

#define ESP_UART          uart1
#define ESP_UART_BAUD     3000000u
/* Small enough that one frame is a fraction of a millisecond on the wire,
 * large enough that the link is not dominated by framing overhead. */
#define ESP_CHUNK         512u

/* How long to keep retrying fwog_io_dir_apply() while the display comes up.
 * It fails cleanly (250 ms ack timeout) rather than hanging, and the display
 * app is usually servicing the link within a few hundred milliseconds. */
#define IO_APPLY_TRIES    20u
#define IO_APPLY_RETRY_MS 50u

/* The direction word this app needs. Everything else keeps the board default
 * (fwog_io_dir_last(), which is fwog_io_cfg_default() until the first
 * successful apply), so this does not disturb SPI, the antennas, or the
 * breakout I2C pull-ups. */
static void uart_directions(fwog_io_cfg_t *cfg) {
    cfg->uart_tx_out  = true;   /* GPIO 8 driven by UART1 TX */
    cfg->uart_rx_out  = false;  /* GPIO 9 reads UART1 RX */
    cfg->uart_cts_out = false;  /* GPIO 10 reads ESP32 RTS */
    cfg->uart_rts_out = true;   /* GPIO 11 drives ESP32 CTS */
}

static bool apply_uart_directions(void) {
    fwog_io_cfg_t cfg = *fwog_io_dir_last();
    uart_directions(&cfg);

    for (unsigned i = 0; i < IO_APPLY_TRIES; i++) {
        const fwog_io_result_t r = fwog_io_dir_apply(&cfg);
        if (r == FWOG_IO_OK) return true;
        board_watchdog_kick();
        sleep_ms(IO_APPLY_RETRY_MS);
    }
    return false;
}

static void esp_uart_init(void) {
    /* set_dir() inside fwog_io_dir_apply() ends with gpio_init(), which
     * selects SIO -- so the UART function must be selected after, never
     * before. */
    uart_init(ESP_UART, ESP_UART_BAUD);
    gpio_set_function(PIN_IO_UART_TX,  GPIO_FUNC_UART);
    gpio_set_function(PIN_IO_UART_RX,  GPIO_FUNC_UART);
    gpio_set_function(PIN_IO_UART_CTS, GPIO_FUNC_UART);
    gpio_set_function(PIN_IO_UART_RTS, GPIO_FUNC_UART);

    /* Idle-high inputs. Without these an unplugged ESP32 leaves RX and CTS
     * floating, which shows up as a permanent break / permanent "not clear
     * to send". */
    gpio_pull_up(PIN_IO_UART_RX);
    gpio_pull_up(PIN_IO_UART_CTS);

    uart_set_hw_flow(ESP_UART, true, true);
    uart_set_format(ESP_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(ESP_UART, true);
}

static void mirror_to_usb(const uint8_t *data, size_t len) {
#if FWOG_DIAG == FWOG_DIAG_USB
    /* pico_stdio_usb drops output when nothing is attached, but when a host
     * is attached and not reading it can stall for up to 2 s. The LCD is the
     * primary output, so do not let a busy USB host hold up the frame path:
     * a disconnected port costs nothing here. */
    if (!stdio_usb_connected()) return;
#endif
    (void)fwrite(data, 1, len, stdout);
    (void)fflush(stdout);
}

/* Put one line on the display before any ESP32 byte exists, so a board with
 * nothing connected says so on the panel instead of looking dead. */
static void link_say(const char *s) {
    uint8_t frame[96];
    size_t n = strlen(s);
    if (n > sizeof frame - 1u) n = sizeof frame - 1u;
    frame[0] = ESPUART_MSG_DATA;
    memcpy(frame + 1, s, n);
    (void)fwog_link_uart_send_frame(frame, n + 1u);
}

int main(void) {
    board_init();

    /* Brings the display up: link, HELLO, reflash if needed, then RUN.
     * Failures are reported but not fatal -- if the display half is missing
     * this still acts as a USB CDC monitor. */
    const fwog_display_result_t d = fwog_display_update_run();

    const bool pads_ok = apply_uart_directions();
    esp_uart_init();

    /* Switching the pad mux can leave one partial byte in the receiver.
     * Drop it before it becomes a stray glyph on the display; anything the
     * ESP32 sends after this point is real. */
    while (uart_is_readable(ESP_UART)) (void)uart_getc(ESP_UART);

    const uint32_t actual = fwog_uart_actual_baud(
        (uint32_t)clock_get_hz(clk_peri), ESP_UART_BAUD);

    /* USB CDC drops output until the host asserts DTR, which happens a
     * second or so after the device enumerates. Wait briefly -- kicking the
     * watchdog while doing so -- then say this once and never again: the
     * same port carries the raw ESP32 mirror, so a periodic heartbeat here
     * would corrupt the stream it is mirroring. */
    for (unsigned i = 0; i < 20; i++) {
        board_watchdog_kick();
        sleep_ms(100);
    }
    link_say(pads_ok
                 ? "espuart ready. Waiting for ESP32 UART 8N1 3M."
                 : "espuart: UART pads FAILED. See main CDC.");
    DIAG("[espuart_main] display: %s, uart pads: %s, uart1 %u baud "
         "(requested %u)\n",
         fwog_display_result_text(d), pads_ok ? "ok" : "FAILED",
         (unsigned)actual, (unsigned)ESP_UART_BAUD);

    uint8_t chunk[ESP_CHUNK];
    uint8_t frame[ESP_CHUNK + 1u];
    frame[0] = ESPUART_MSG_DATA;

    while (true) {
        board_watchdog_kick();

        size_t n = 0;
        while (n < sizeof chunk && uart_is_readable(ESP_UART)) {
            chunk[n++] = (uint8_t)uart_getc(ESP_UART);
        }

        if (n == 0u) {
            sleep_us(100);      /* nothing to do; keep the loop cheap */
            continue;
        }

        memcpy(frame + 1, chunk, n);
        /* Payload cap is FWOG_LINK_MAX_PAYLOAD (4160), far above ESP_CHUNK. */
        (void)fwog_link_uart_send_frame(frame, n + 1u);
        mirror_to_usb(chunk, n);
    }
}
