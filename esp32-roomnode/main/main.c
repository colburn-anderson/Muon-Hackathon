/* Room node, network side.
 *
 * The C6 owns the network and nothing else. It publishes what the main CPU
 * senses and forwards what the backend commands; the room's state machine, the
 * sensor and the servo all live on the main RP2040.
 *
 * Console is on the USB Serial/JTAG (the ESP32's own USB port), NOT on UART0:
 * UART0 is the 115200-baud link to the main CPU, and one ESP_LOG line down that
 * wire would corrupt a JSON line. Logs and protocol never share a wire here.
 */

#include "roomnode_config.h"
#include "api.h"
#include "console.h"
#include "link.h"
#include "wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "roomnode";

void app_main(void) {
    /* NVS holds the Wi-Fi calibration data; without it the first association
     * after boot silently uses defaults and a persistent failure looks random. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "fwog room node: room %s", ROOM_ID);
    ESP_LOGI(TAG, "api %s/device/rooms/%s, key %s", API_BASE, ROOM_ID,
             DEVICE_KEY[0] ? "(set)" : "(MISSING)");

    /* Link first: the main CPU may be talking already, and its boot line is
     * the cheapest evidence that the wire is good. */
    rn_link_init();
    rn_link_on_presence(rn_api_note_presence);
    rn_link_on_end(rn_api_end);
    rn_link_on_boot(rn_api_note_main_boot);
    xTaskCreate(&rn_link_task, "link", 4096, NULL, 6, NULL);

    rn_wifi_start();
    rn_api_start();
    rn_console_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        rn_link_stats_t link_stats;
        rn_link_get_stats(&link_stats);
        ESP_LOGI(TAG, "alive, wifi %s, heap %u, link tx lines=%lu bytes=%lu short=%lu pin=%d",
                 rn_wifi_up() ? "up" : "down",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned long)link_stats.tx_lines,
                 (unsigned long)link_stats.tx_bytes,
                 (unsigned long)link_stats.tx_short_writes,
                 link_stats.tx_pin_high ? 1 : 0);
    }
}
