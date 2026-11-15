#include "console.h"

#include "api.h"
#include "link.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "console";

static void print_help(void) {
    ESP_LOGI(TAG, "commands: mock free|held|reserved|warn|unknown [seconds]; mock off; servo 0..100");
}

static void console_task(void *arg) {
    (void)arg;
    char line[96];

    print_help();
    while (true) {
        if (fgets(line, sizeof line, stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';

        char state[16] = "";
        int seconds = 0;
        int servo = 0;
        if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            print_help();
        } else if (sscanf(line, "servo %d", &servo) == 1) {
            if (servo < 0 || servo > 100) {
                ESP_LOGW(TAG, "servo must be 0..100");
            } else {
                rn_link_cmd(servo);
                ESP_LOGI(TAG, "servo test -> %d%%", servo);
            }
        } else if (strcmp(line, "mock off") == 0) {
            rn_api_mock_off();
        } else if (sscanf(line, "mock %15s %d", state, &seconds) >= 1) {
            if (!rn_api_mock_state(state, seconds)) {
                ESP_LOGW(TAG, "unknown mock state: %s", state);
                print_help();
            }
        } else if (line[0] != '\0') {
            ESP_LOGW(TAG, "unknown command: %s", line);
            print_help();
        }
    }
}

void rn_console_start(void) {
    xTaskCreate(&console_task, "console", 3072, NULL, 3, NULL);
}
