/* Minimal HTTP/1.1 client with optional forward-proxy support.
 *
 * Why not esp_http_client: the IDF v6.1 config struct has no proxy_host /
 * proxy_port fields, so a proxied bench network cannot be reached with it.
 * This speaks the same absolute-URI dialect a proxy expects, over a plain
 * socket, which is about sixty lines and works. */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      status;        /* HTTP status code, or negative on transport error */
    char     body[768];     /* NUL-terminated response body (truncated if longer) */
    int64_t  server_epoch;  /* from the Date header, 0 if absent/unparseable */
} rn_http_resp_t;

/* Performs METHOD on URL. BODY may be NULL for GET. Always fills OUT.
 * Returns 0 when a response was received (whatever its status), negative on a
 * transport-level failure, in which case resp->status carries the errno-ish
 * value. */
int rn_http_do(const char *method, const char *url, const char *body,
               rn_http_resp_t *out);
