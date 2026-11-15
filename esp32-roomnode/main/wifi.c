#include "wifi.h"
#include "roomnode_config.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

static volatile bool s_up;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_up = false;
        ESP_LOGW(TAG, "disconnected (reason %d), retrying in %d ms",
                 d != NULL ? d->reason : -1, WIFI_RETRY_MS);
        /* Not esp_wifi_connect() straight away: an AP that rejects us for a
         * reason that will not change (205 wrong password, 201 no AP) turns
         * an immediate retry into a spin. */
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_MS));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_up = true;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

void rn_wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof cfg.sta.ssid - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof cfg.sta.password - 1);
    /* WPA2 is the floor; the tether is WPA2-PSK. Lowering this to OPEN makes
     * a mistyped password look like a successful association. */
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "station started, ssid \"%s\"", WIFI_SSID);
}

bool rn_wifi_up(void) { return s_up; }
