/* The room node's conversation with the backend.
 *
 * Contract (Room Lock API, Nov 2026): the device authenticates with x-device-key
 * and uses only the /device/ endpoints --
 *
 *   GET  /device/rooms/{id}           -> Room, poll every 5 s
 *   POST /device/rooms/{id}/presence  {"present":true} | {"present":false}
 *                                     on every change, plus every 60 s
 *   POST /device/rooms/{id}/end       "I'm done" button
 *
 * The device does NOT claim, check in or book. Those are web-session verbs; on
 * this side a person is detected and reported, and the server decides whether
 * that starts, extends or ends a session. The first version of this file
 * guessed at claim/checkin/end on presence edges, which was wrong in an
 * expensive way: it would have tried to assert a login it does not have.
 *
 * Errors arrive as plain text ("Room is already held") with a meaningful status
 * (401/403/404/409), so the body is logged verbatim rather than parsed. */

#include "api.h"
#include "link.h"
#include "net.h"
#include "jhelp.h"
#include "roomnode_config.h"

#include <string.h>
#include <stdio.h>

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

/* Last thing main was told, so a steady room is not re-sent every poll. */
static rn_state_t s_last_state = RN_ST_UNKNOWN;
static char      s_last_holder[48];
static int64_t   s_last_expires = -1;
static int64_t   s_last_next_res = -1;
static int       s_last_servo = -1; /* percent sent to main: 0 or 90 */
static char      s_last_alert[128];
static int64_t   s_last_get_ok_ms;
static bool      s_offline_lock_sent;

/* What the sensor has told us, and what the API has been told about it. */
static bool      s_present;
static bool      s_reported_present;
static bool      s_reported_valid;
static int64_t   s_last_presence_post_ms;

static int64_t   s_time_synced;
static int64_t   s_epoch_at_sync;
static int64_t   s_sync_uptime_ms;

/* Requests arrive from the UART/console tasks; the API task owns all network
 * calls. The mock changes only what is published to main, never backend data. */
static volatile bool       s_end_requested;
static volatile bool       s_mock_active;
static volatile rn_state_t s_mock_state;
static volatile int64_t    s_mock_expires_at;

static int64_t uptime_ms(void) {
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int64_t wall_now(void) {
    if (s_epoch_at_sync <= 0) {
        return 0;
    }
    return s_epoch_at_sync + (uptime_ms() - s_sync_uptime_ms) / 1000;
}

static void publish_mock(void) {
    rn_link_status(state_name(s_mock_state), "USB mock", "",
                   s_mock_expires_at, 0);
    const int64_t now = wall_now();
    if (now > 0) {
        rn_link_time(now);
    }
}

static void url_room(char *out, size_t sz) {
    snprintf(out, sz, API_BASE "/device/rooms/" ROOM_ID);
}

static void url_sub(char *out, size_t sz, const char *sub) {
    snprintf(out, sz, API_BASE "/device/rooms/" ROOM_ID "/%s", sub);
}

/* Applies one successful GET to the link, only for the parts that changed. */
static void publish_response(const rn_http_resp_t *resp) {
    char state[16] = "unknown";
    char holder[48] = "";
    char url[768] = "";
    double exp_d = 0, nxt_d = 0;

    jh_string(resp->body, "state", state, sizeof state);
    /* holder is deliberately absent from device responses: the device is not
     * the holder and is not "yours", so there is usually nobody to name. The
     * parse stays because a future revision may include it, and an empty
     * string is the correct display for "nobody". */
    jh_string(resp->body, "holder", holder, sizeof holder);
    jh_string(resp->body, "url", url, sizeof url);
    if (strlen(url) >= 192u) {
        ESP_LOGW(TAG, "room URL is %u bytes; maximum is 191",
                 (unsigned)strlen(url));
        url[0] = '\0';
    }
    jh_number(resp->body, "expires_at", &exp_d);
    jh_number(resp->body, "next_res_start", &nxt_d);

    const rn_state_t st = parse_state(state);
    const int64_t exp_at = (int64_t)exp_d;
    const int64_t nxt = (int64_t)nxt_d;

    /* Status is intentionally repeated after every successful poll. Main can
     * reset or the physical UART can be reconnected while the room remains
     * unchanged; change-only publication would then leave the display at
     * UNKNOWN indefinitely. A USB mock overrides only this publication; the
     * real poll, presence reports and clock synchronization continue. */
    if (s_mock_active) {
        publish_mock();
    } else {
        rn_link_status(state_name(st), holder, url, exp_at, nxt);
    }
    if (st != s_last_state || strcmp(holder, s_last_holder) != 0 ||
        exp_at != s_last_expires || nxt != s_last_next_res) {
        ESP_LOGI(TAG, "room %s: %s expires=%lld next=%lld", ROOM_ID, state,
                 (long long)exp_at, (long long)nxt);
        s_last_state = st;
        snprintf(s_last_holder, sizeof s_last_holder, "%s", holder);
        s_last_expires = exp_at;
        s_last_next_res = nxt;
    }

    /* The server's view of our own presence, for cross-checking. If it disagrees
     * with the sensor, that is worth a log line -- it means a POST was lost. */
    bool server_present = false;
    if (jh_bool(resp->body, "presence", &server_present) &&
        server_present != s_present) {
        ESP_LOGW(TAG, "server presence %d, sensor says %d",
                 server_present ? 1 : 0, s_present ? 1 : 0);
    }

    /* Service contract: every full room response carries servo 0|1. Missing
     * means locked, so an older/partial response fails secure. The physical
     * simulation uses 90% rather than the driver's full 100% endpoint. */
    double servo_d = 0;
    const bool have_servo = jh_number(resp->body, "servo", &servo_d);
    const int servo_value = have_servo ? (int)servo_d : 0;
    if (have_servo && servo_value != 0 && servo_value != 1) {
        ESP_LOGW(TAG, "invalid servo value %d; locking", servo_value);
    }
    const int servo_percent = servo_value == 1 ? 90 : 0;
    if (servo_percent != s_last_servo) {
        s_last_servo = servo_percent;
        rn_link_cmd(servo_percent);
        ESP_LOGI(TAG, "servo %d -> %d%%", servo_value, servo_percent);
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
     * seconds. Every response carries a Date header, so this costs nothing. */
    if (resp->server_epoch > 0 &&
        (s_time_synced == 0 || llabs(resp->server_epoch - s_time_synced) >= 5)) {
        s_time_synced = resp->server_epoch;
        s_epoch_at_sync = resp->server_epoch;
        s_sync_uptime_ms = uptime_ms();
        rn_link_time(s_epoch_at_sync);
        ESP_LOGI(TAG, "time synced: %lld", (long long)s_epoch_at_sync);
    }
}

static bool poll_once(void) {
    char url[192];
    url_room(url, sizeof url);

    rn_http_resp_t resp;
    if (rn_http_do("GET", url, NULL, &resp) != 0 || resp.status <= 0) {
        ESP_LOGW(TAG, "GET %s failed", url);
        rn_link_net(false, "unreachable");
        return false;
    }
    if (resp.status != 200) {
        /* Plain-text error body, per the contract. Log verbatim: 403 is the one
         * that means the device key is wrong, and it reads "Device key
         * required", which is worth seeing exactly once. */
        ESP_LOGW(TAG, "GET %s -> %d %s", url, resp.status, resp.body);
        rn_link_net(resp.status != 401 && resp.status != 403,
                    resp.status == 403 ? "bad device key" : "error");
        return false;
    }

    rn_link_net(true, "ok");
    s_last_get_ok_ms = uptime_ms();
    s_offline_lock_sent = false;
    publish_response(&resp);
    return true;
}

/* Reports presence. Sent on every change and every PRESENCE_RESEND_S, so a lost
 * packet cannot silently end a session ten minutes later. */
static void post_presence(bool present) {
    char url[192];
    url_sub(url, sizeof url, "presence");

    const char *body = present ? "{\"present\":true}" : "{\"present\":false}";

    rn_http_resp_t resp;
    if (rn_http_do("POST", url, body, &resp) != 0) {
        ESP_LOGW(TAG, "presence POST transport failure");
        return;
    }
    if (resp.status >= 200 && resp.status < 300) {
        ESP_LOGI(TAG, "presence %s -> %d", present ? "true" : "false", resp.status);
        s_reported_present = present;
        s_reported_valid = true;
        s_last_presence_post_ms = uptime_ms();
        publish_response(&resp);
    } else {
        /* 409 here is a room rule refusing the report, not our bug: log the
         * server's own words and let the next poll show the real state. */
        ESP_LOGW(TAG, "presence -> %d %s", resp.status, resp.body);
    }
}

void rn_api_note_presence(bool present) {
    if (present == s_present) {
        return;
    }
    s_present = present;
    ESP_LOGI(TAG, "sensor presence %s", present ? "began" : "ended");
}

/* main rebooted, so nothing it was told survives. Invalidate the caches that
 * exist to avoid repeating ourselves -- above all the servo, because main comes
 * up locked: if the service says unlocked and the cache still matches, the door
 * would stay locked until the service happened to change its mind. */
void rn_api_note_main_boot(void) {
    s_last_servo = -1;
    s_last_state = RN_ST_UNKNOWN;
    s_last_expires = -1;
    s_last_next_res = -1;
    s_last_alert[0] = '\0';
    s_offline_lock_sent = false;
    ESP_LOGI(TAG, "main rebooted: re-sending service state on the next poll");
}

void rn_api_end(void) {
    if (s_mock_active) {
        s_mock_state = RN_ST_FREE;
        s_mock_expires_at = 0;
        publish_mock();
        ESP_LOGI(TAG, "USB mock: end -> free (real endpoint not called)");
        return;
    }
    s_end_requested = true;
    ESP_LOGI(TAG, "end requested");
}

static void post_end(void) {
    char url[192];
    url_sub(url, sizeof url, "end");

    rn_http_resp_t resp;
    if (rn_http_do("POST", url, "", &resp) != 0) {
        ESP_LOGW(TAG, "end: transport failure");
        return;
    }
    if (resp.status >= 200 && resp.status < 300) {
        ESP_LOGI(TAG, "end ok (%d)", resp.status);
        publish_response(&resp);
    } else {
        ESP_LOGW(TAG, "end -> %d %s", resp.status, resp.body);
    }
}

bool rn_api_mock_state(const char *state, int seconds) {
    const rn_state_t parsed = parse_state(state);
    if (parsed == RN_ST_UNKNOWN && strcmp(state, "unknown") != 0) {
        return false;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    s_mock_state = parsed;
    const int64_t now = wall_now();
    s_mock_expires_at = (seconds > 0 && now > 0) ? now + seconds : 0;
    s_mock_active = true;
    publish_mock();
    ESP_LOGI(TAG, "USB mock: %s for %ds", state_name(parsed), seconds);
    return true;
}

void rn_api_mock_off(void) {
    s_mock_active = false;
    ESP_LOGI(TAG, "USB mock disabled; next poll publishes backend state");
}

static void api_task(void *arg) {
    (void)arg;

    while (true) {
        const bool get_ok = poll_once();
        const int64_t now_ms = uptime_ms();
        if (!get_ok && !s_offline_lock_sent &&
            now_ms - s_last_get_ok_ms >= OFFLINE_LOCK_MS) {
            rn_link_cmd(0);
            s_last_servo = 0;
            s_offline_lock_sent = true;
            ESP_LOGW(TAG, "no successful GET for 60s: forcing servo locked");
        }

        /* Report on change, plus a periodic reminder. The resend matters more
         * than it looks: absence is what ends a session, so one dropped POST
         * must not be able to look like an empty room. */
        const bool changed = !s_reported_valid || s_present != s_reported_present;
        const bool stale = s_reported_valid &&
                           uptime_ms() - s_last_presence_post_ms >= PRESENCE_RESEND_MS;
        if (changed || stale) {
            post_presence(s_present);
        }

        if (s_end_requested) {
            s_end_requested = false;
            post_end();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void rn_api_start(void) {
    xTaskCreate(&api_task, "api", 8192, NULL, 5, NULL);
}
