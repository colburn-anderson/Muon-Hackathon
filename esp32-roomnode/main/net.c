#include "net.h"
#include "roomnode_config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "net";

#define NET_RECV_TIMEOUT_S 6

/* "2026-11-14T22:04:46Z"-shaped Date headers are RFC1123 in practice
 * ("Fri, 14 Nov 2026 03:04:46 GMT"). newlib here has no strptime unless it is
 * asked for, so parse the four fields we need by hand. */
static int64_t parse_http_date(const char *value) {
    /* Formatted pieces we must see: ", DD Mon YYYY HH:MM:SS GMT" */
    const char *p = strchr(value, ',');
    p = (p != NULL) ? p + 1 : value;

    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon[4] = {0};
    if (sscanf(p, " %d %3s %d %d:%d:%d", &day, mon, &year, &hour, &min, &sec) != 6) {
        return 0;
    }

    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, months[i], 3) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0 || year < 1970) {
        return 0;
    }

    /* Days since the epoch, civil-from-days (Howard Hinnant's algorithm). */
    int y = year - (month <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;

    return days * 86400 + hour * 3600 + min * 60 + sec;
}

/* Splits http://host[:port]/path. Returns false on anything else. */
static bool split_url(const char *url, char *host, size_t hostsz,
                      int *port, const char **path) {
    const char *p = strstr(url, "://");
    if (p == NULL) {
        return false;
    }
    if (strncmp(url, "http://", 7) != 0) {
        /* https:// through the proxy needs CONNECT; direct TLS is out of
         * scope for a placeholder backend. Fail where it is visible. */
        ESP_LOGE(TAG, "only http:// is supported (%s)", url);
        return false;
    }
    p += 3;

    const char *slash = strchr(p, '/');
    size_t hostlen = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
    const char *colon = memchr(p, ':', hostlen);

    size_t copy = (colon != NULL) ? (size_t)(colon - p) : hostlen;
    if (copy == 0 || copy >= hostsz) {
        return false;
    }
    memcpy(host, p, copy);
    host[copy] = '\0';

    *port = 80;
    if (colon != NULL) {
        char portbuf[8] = {0};
        size_t plen = hostlen - copy - 1;
        if (plen == 0 || plen >= sizeof portbuf) {
            return false;
        }
        memcpy(portbuf, colon + 1, plen);
        *port = atoi(portbuf);
    }

    *path = (slash != NULL) ? slash : "/";
    return true;
}

static int connect_to(const char *host, int port) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    char service[8];
    snprintf(service, sizeof service, "%d", port);

    if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
        ESP_LOGE(TAG, "dns failed for %s", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = {.tv_sec = NET_RECV_TIMEOUT_S, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect %s:%d failed", host, port);
        close(fd);
        return -1;
    }
    return fd;
}

int rn_http_do(const char *method, const char *url, const char *body,
               rn_http_resp_t *out) {
    memset(out, 0, sizeof *out);
    out->status = -1;

    char host[64];
    int port = 80;
    const char *path = NULL;
    if (!split_url(url, host, sizeof host, &port, &path)) {
        return -1;
    }

    /* Who we actually open a socket to, and what goes in the request line. */
    const char *conn_host = USE_PROXY ? PROXY_HOST : host;
    int conn_port = USE_PROXY ? PROXY_PORT : port;

    int fd = connect_to(conn_host, conn_port);
    if (fd < 0) {
        out->status = -1;
        return -1;
    }

    size_t body_len = (body != NULL) ? strlen(body) : 0;

    char head[512];
    size_t off = 0;
#define HEAD_APPEND(...)                                                      \
    do {                                                                      \
        int _w = snprintf(head + off, (off < sizeof head) ? sizeof head - off : 0, \
                          __VA_ARGS__);                                       \
        if (_w > 0) off += (size_t)_w;                                         \
    } while (0)

    if (USE_PROXY) {
        /* An absolute URI in the request line is what makes this a proxy
         * request rather than an origin request. */
        HEAD_APPEND("%s http://%s:%d%s HTTP/1.1\r\n", method, host, port, path);
    } else {
        HEAD_APPEND("%s %s HTTP/1.1\r\n", method, path);
    }
    HEAD_APPEND("Host: %s:%d\r\n", host, port);
    HEAD_APPEND("Authorization: Bearer " API_TOKEN "\r\n");
    if (body_len > 0) {
        HEAD_APPEND("Content-Type: application/json\r\n");
        HEAD_APPEND("Content-Length: %u\r\n", (unsigned)body_len);
    }
    HEAD_APPEND("User-Agent: fwog-roomnode\r\n");
    HEAD_APPEND("Connection: close\r\n\r\n");
#undef HEAD_APPEND

    if (off == 0 || off >= sizeof head) {
        close(fd);
        return -1;
    }

    size_t sent = 0;
    size_t head_len = strlen(head);
    while (sent < head_len) {
        int w = send(fd, head + sent, head_len - sent, 0);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)w;
    }
    sent = 0;
    while (sent < body_len) {
        int w = send(fd, body + sent, body_len - sent, 0);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)w;
    }

    /* Read the whole response. Connection: close means the server ends it. */
    static char raw[1600];
    size_t len = 0;
    while (len < sizeof raw - 1) {
        int r = recv(fd, raw + len, sizeof raw - 1 - len, 0);
        if (r <= 0) {
            break;
        }
        len += (size_t)r;
    }
    close(fd);
    raw[len] = '\0';

    if (len == 0) {
        out->status = -1;
        return -1;
    }

    /* Status line: HTTP/1.1 204 No Content */
    const char *sp = strchr(raw, ' ');
    out->status = (sp != NULL) ? atoi(sp + 1) : -1;

    /* Split headers from body. */
    char *sep = strstr(raw, "\r\n\r\n");
    char *body_start = (sep != NULL) ? sep + 4 : raw + len;

    /* Date header, for a wall clock the RP2040s do not have. */
    for (char *line = strstr(raw, "\r\n"); line != NULL && line < body_start;
         line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, "date:", 5) == 0) {
            out->server_epoch = parse_http_date(line + 7);
            break;
        }
    }

    size_t blen = (size_t)(raw + len - body_start);
    if (blen >= sizeof out->body) {
        blen = sizeof out->body - 1;
    }
    memcpy(out->body, body_start, blen);
    out->body[blen] = '\0';

    return 0;
}
