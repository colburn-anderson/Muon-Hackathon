#include "api.h"
#include "link.h"
#include "net.h"
#include "jhelp.h"
#include "roomnode_config.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "api";

typedef enum {
    RN_ST_UNKNOWN = 0,
    RN_ST_FREE,
    RN_ST_HELD,
    RN_ST_RESERVED,
    RN_ST_WARN,
} rn_state_t;

static const char *state_name(rn_state_t s) {
    switch (s) {
    case RN_ST_FREE:     return "free";
    case RN_ST_HELD:     return "held";
    case RN_ST_RESERVED: return "reserved";
    case RN_ST_WARN:     return "warn";
    default:             return "unknown";
    }
}

static rn_state_t parse_state(const char *s) {
    if (s == NULL)                  return RN_ST_UNKNOWN;
    if (strcmp(s, "free") == 0)     return RN_ST_FREE;
    if (strcmp(s, "held") == 0)     return RN_ST_HELD;
    if (strcmp(s, "reserved") == 0) return RN_ST_RESERVED;
    if (strcmp(s, "warn") == 0)     return RN_ST_WARN;
    return RN_ST_UNKNOWN;
}

/* Last thing the other side was told, so a steady state is not re-sent every
 * two seconds just because we polled. */
static rn_state_t s_last_state = RN_ST_UNKNOWN;
static char      s_last_holder[48];
static int64_t   s_last_expires;
static int64_t   s_last_next_res;
static int       s_last_servo = -1;
static char      s_last_alert[128];

static bool      s_present;
static bool      s_reported_present;   /* what the API has been told */
static int64_t   s_time_synced;
static int64_t   s_epoch_at_sync;
static int64_t   s_sync_uptime_ms;

static int64_t uptime_ms(void) {
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void url_room(char *out, size_t sz) {
    snprintf(out, sz, API_BASE "/rooms/" ROOM_ID);
}

static void url_action(char *out, size_t sz, const char *action) {
    snprintf(out, sz, API_BASE "/rooms/" ROOM_ID "/%s", action);
}

/* Applies one successful GET to the link, only for the parts that changed. */
static void publish(const rn_http_resp_t *resp) {
    char state[16] = "unknown";
    char holder[48] = "";
    double exp_d = 0, nxt_d = 0;

    jh_string(resp->body, "state", state, sizeof state);
    jh_string(resp->body, "holder", holder, sizeof holder);
    jh_number(resp->body, "expires_at", &exp_d);
    jh_number(resp->body, "next_res_start", &nxt_d);

    const rn_state_t st = parse_state(state);
    const int64_t exp_at = (int64_t)exp_d;
    const int64_t nxt = (int64_t)nxt_d;

    if (st != s_last_state || strcmp(holder, s_last_holder) != 0 ||
        exp_at != s_last_expires || nxt != s_last_next_res) {
        rn_link_status(state_name(st), holder, exp_at, nxt);
        s_last_state = st;
        snprintf(s_last_holder, sizeof s_last_holder, "%s", holder);
        s_last_expires = exp_at;
        s_last_next_res = nxt;
    }

    /* Commands. These are our assumption about the contract until the backend
     * confirms them: a top-level "servo" angle and {"alert":{"text","level"}}.
     * An absent field means "no change", never "reset to default". */
    double servo_d = 0;
    if (jh_number(resp->body, "servo", &servo_d)) {
        const int angle = (int)servo_d;
        if (angle != s_last_servo) {
            s_last_servo = angle;
            rn_link_cmd(angle);
            ESP_LOGI(TAG, "servo -> %d", angle);
        }
    }

    const char *alert_obj = jh_object(resp->body, "alert");
    if (alert_obj != NULL) {
        char text[128] = "";
        double level_d = 1;
        jh_string(alert_obj, "text", text, sizeof text);
        jh_number(alert_obj, "level", &level_d);
        if (strcmp(text, s_last_alert) != 0) {
            snprintf(s_last_alert, sizeof s_last_alert, "%s", text);
            rn_link_alert(text, (int)level_d);
            ESP_LOGI(TAG, "alert(%d): %s", (int)level_d, text);
        }
    } else if (jh_has(resp->body, "alert") && s_last_alert[0] != '\0') {
        /* Explicit null clears an existing banner; a missing key leaves it. */
        s_last_alert[0] = '\0';
        rn_link_alert("", 0);
    }

    /* Wall clock for the RP2040s: they have none, and the API speaks epoch
     * seconds. The Date header rides along on every response for free. */
    if (resp->server_epoch > 0 &&
        (s_time_synced == 0 || llabs(resp->server_epoch - s_time_synced) >= 5)) {
        s_time_synced = resp->server_epoch;
        s_epoch_at_sync = resp->server_epoch;
        s_sync_uptime_ms = uptime_ms();
        rn_link_time(s_epoch_at_sync);
        ESP_LOGI(TAG, "time synced: %lld", (long long)s_epoch_at_sync);
    }
}

static void poll_once(void) {
    char url[192];
    url_room(url, sizeof url);

    rn_http_resp_t resp;
    if (rn_http_do("GET", url, NULL, &resp) != 0 || resp.status <= 0) {
        ESP_LOGW(TAG, "GET %s failed", url);
        rn_link_net(false, "unreachable");
        return;
    }
    if (resp.status != 200) {
        /* Plain-text error body, per the contract. Log verbatim; a 401 is the
         * only one a human can act on, and the display shows it as a banner. */
        ESP_LOGW(TAG, "GET %s -> %d %s", url, resp.status, resp.body);
        rn_link_net(resp.status != 401, resp.status == 401 ? "unauthorized" : "error");
        return;
    }

    rn_link_net(true, "ok");
    publish(&resp);
}

/* Fires one of claim/checkin/end. Body is empty; the room id is in the path
 * and the identity is in the bearer token. */
static void action(const char *name) {
    char url[192];
    url_action(url, sizeof url, name);

    rn_http_resp_t resp;
    if (rn_http_do("POST", url, "", &resp) != 0) {
        ESP_LOGW(TAG, "%s: transport failure", name);
        return;
    }
    if (resp.status >= 200 && resp.status < 300) {
        ESP_LOGI(TAG, "%s ok (%d)", name, resp.status);
    } else {
        ESP_LOGW(TAG, "%s -> %d %s", name, resp.status, resp.body);
    }
}

void rn_api_note_presence(bool present) {
    if (present == s_present) {
        return;
    }
    s_present = present;
    ESP_LOGI(TAG, "presence %s", present ? "began" : "ended");
}

static void api_task(void *arg) {
    (void)arg;
    int64_t absent_since = 0;

    while (true) {
        poll_once();

        /* Rising edge: tell the backend once. A free room gets claimed; an
         * already held or reserved one gets a check-in -- same intent (a body
         * is in the room), different server policy. */
        if (s_present && !s_reported_present) {
            s_reported_present = true;
            absent_since = 0;
            action(s_last_state == RN_ST_FREE ? "claim" : "checkin");
        }

        /* Falling edge, but only after the room has been quiet long enough
         * that "popped out for a coffee" does not end a reservation. */
        if (!s_present) {
            if (absent_since == 0) {
                absent_since = uptime_ms();
            }
            if (s_reported_present &&
                uptime_ms() - absent_since >= ABSENT_TIMEOUT_S * 1000LL) {
                s_reported_present = false;
                action("end");
            }
        } else {
            absent_since = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void rn_api_start(void) {
    xTaskCreate(&api_task, "api", 8192, NULL, 5, NULL);
}
