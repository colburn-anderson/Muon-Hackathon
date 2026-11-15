/* HTTPS client for the room node, through the tether's forward proxy.
 *
 * Why this is not esp_http_client. The real API is HTTPS only, and the only
 * route off this hotspot is an HTTP proxy at 192.168.49.1:8228 -- which means
 * every request needs a CONNECT tunnel first. Neither esp_http_client nor
 * esp-tls has a proxy option in IDF v6.1 (I checked their headers, not the
 * docs), so the choice is: write the tunnel, or have no working device.
 *
 * The shape is therefore: plain TCP to the proxy -> "CONNECT host:443" -> read
 * the 200 -> hand the same socket to mbedtls -> speak HTTP/1.1 myself. That
 * costs about two hundred lines and buys two accidental things:
 *
 *   - No DNS on the C6. The proxy resolves the hostname, which matters because
 *     this hotspot hands out no usable resolver (getaddrinfo() failed with 202
 *     while the same URL worked from the laptop).
 *   - Certificate verification is still the real thing: mbedtls checks the
 *     chain against IDF's bundle, which carries Amazon Root CA 1.
 *
 * Set USE_PROXY 0 on a network with a working resolver and direct egress; the
 * code path then dials the origin directly and everything else is unchanged. */

#include "net.h"
#include "roomnode_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"

#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"

static const char *TAG = "net";

#define IO_TIMEOUT_S 8

typedef struct {
    char   host[96];
    char   path[160];
    int    port;
} url_t;

static bool parse_url(const char *url, url_t *out) {
    memset(out, 0, sizeof *out);
    const char *rest = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        rest = url + 8;
        out->port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        rest = url + 7;
        out->port = 80;
    } else {
        return false;
    }

    const char *slash = strchr(rest, '/');
    const size_t hostlen = slash ? (size_t)(slash - rest) : strlen(rest);
    if (hostlen == 0 || hostlen >= sizeof out->host) {
        return false;
    }
    memcpy(out->host, rest, hostlen);
    out->host[hostlen] = '\0';
    snprintf(out->path, sizeof out->path, "%s", slash ? slash : "/");

    char *colon = strrchr(out->host, ':');
    if (colon != NULL) {
        out->port = atoi(colon + 1);
        *colon = '\0';
    }
    return out->port > 0;
}

static int tcp_connect(const char *host, int port) {
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    const int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGW(TAG, "getaddrinfo(%s) failed: %d", host, gai);
        return -1;
    }

    const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* Without these a dead peer parks this task for the lwip default, which is
     * forever, and the display shows a stale room for the rest of the day. */
    struct timeval tv = { .tv_sec = IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    const int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect(%s:%d) failed: errno %d", host, port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

/* Sends "CONNECT host:port" and waits for a 2xx. Anything else is the proxy
 * refusing, and the reply body says why ("Host not allowed" and friends). */
static bool proxy_tunnel(int fd, const char *host, int port) {
    char req[192];
    int n = snprintf(req, sizeof req,
                     "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Proxy-Connection: keep-alive\r\n\r\n",
                     host, port, host, port);
    if (send(fd, req, (size_t)n, 0) != n) {
        return false;
    }

    char resp[256];
    size_t len = 0;
    while (len < sizeof resp - 1) {
        const int r = recv(fd, resp + len, 1, 0);
        if (r <= 0) {
            return false;
        }
        len++;
        if (len >= 4 && memcmp(resp + len - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    resp[len] = '\0';
    if (strstr(resp, " 200") == NULL) {
        ESP_LOGW(TAG, "proxy refused tunnel: %s", resp);
        return false;
    }
    return true;
}

/* TLS over an already-connected socket. mbedtls rather than esp-tls because
 * esp-tls insists on doing its own connect(); this is the documented way to
 * re-use a socket, and esp_crt_bundle_attach() is an mbedtls verify callback,
 * so the bundle works here unchanged. */
/* No entropy/ctr_drbg members: this is mbedtls 4, where TLS takes its RNG from
 * PSA and mbedtls_ssl_conf_rng() is gone. psa_crypto_init() is idempotent, so
 * calling it per session is cheaper than tracking it. */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    int                 fd;
} tls_t;

/* Own BIO rather than mbedtls_net_send/recv: it keeps this file independent of
 * the net_sockets module, whose shape has moved between mbedtls majors. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const int fd = *(int *)ctx;
    const int r = send(fd, buf, len, 0);
    if (r < 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const int fd = *(int *)ctx;
    const int r = recv(fd, buf, len, 0);
    if (r < 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return r;
}

static bool tls_start(tls_t *t, int fd, const char *host) {
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    t->fd = fd;

    bool ok = false;
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init failed");
        goto done;
    }
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        goto done;
    }

    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, NULL, NULL);
    if (esp_crt_bundle_attach(&t->conf) != ESP_OK) {
        ESP_LOGW(TAG, "cert bundle attach failed");
        goto done;
    }
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0) {
        goto done;
    }
    if (mbedtls_ssl_set_hostname(&t->ssl, host) != 0) {
        goto done;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->fd, bio_send, bio_recv, NULL);

    int rc;
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char err[96];
            mbedtls_strerror(rc, err, sizeof err);
            ESP_LOGW(TAG, "tls handshake: %s", err);
            goto done;
        }
    }

    const uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
    if (flags != 0) {
        char info[192];
        mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
        ESP_LOGW(TAG, "certificate not verified: %s", info);
        goto done;
    }
    ok = true;

done:
    if (!ok) {
        mbedtls_ssl_free(&t->ssl);
        mbedtls_ssl_config_free(&t->conf);
    }
    return ok;
}

static void tls_stop(tls_t *t) {
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
}

static bool tls_write_all(tls_t *t, const char *buf, size_t len) {
    while (len > 0) {
        const int n = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf, len);
        if (n <= 0) {
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

/* Reads the whole response. We always send "Connection: close", so EOF is the
 * end of the body and there is no need to be clever about keep-alive. */
static int tls_read_all(tls_t *t, char *buf, size_t cap) {
    size_t len = 0;
    while (len < cap - 1) {
        const int n = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf + len, cap - 1 - len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    buf[len] = '\0';
    return (int)len;
}

/* days since 1970-01-01 from a civil date (Hinnant). Here rather than
 * strptime/timegm because newlib's time functions are local-time oriented and
 * the Date header is always GMT. */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2u);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* "Sun, 14 Nov 2026 22:58:56 GMT" -> epoch seconds, 0 if unparseable. */
static int64_t parse_http_date(const char *s) {
    const char *comma = s ? strchr(s, ',') : NULL;
    if (comma == NULL) {
        return 0;
    }
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon[4] = {0};
    if (sscanf(comma + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &hour, &min, &sec) != 6) {
        return 0;
    }
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *at = strstr(months, mon);
    if (at == NULL) {
        return 0;
    }
    const unsigned month = (unsigned)((at - months) / 3) + 1u;
    if (day < 1 || day > 31 || year < 2020 || month > 12u) {
        return 0;
    }
    return days_from_civil(year, month, (unsigned)day) * 86400 +
           (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

/* Copies the raw body into out. API Gateway answers with Content-Length, but
 * "chunked" is one config change away on the server side and a silently
 * truncated body would look like a parse bug in jhelp, so both are handled. */
static void take_body(const char *resp, const char *body, size_t body_cap,
                      bool chunked, char *out) {
    if (!chunked) {
        snprintf(out, body_cap, "%s", body);
        return;
    }
    size_t o = 0;
    const char *p = body;
    while (*p != '\0' && o < body_cap - 1u) {
        char *end = NULL;
        const unsigned long n = strtoul(p, &end, 16);
        if (end == NULL || end == p || n == 0) {
            break;
        }
        p = end;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        for (unsigned long i = 0; i < n && p[i] != '\0' && o < body_cap - 1u; i++) {
            out[o++] = p[i];
        }
        p += n;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
    }
    out[o] = '\0';
    (void)resp;
}

int rn_http_do(const char *method, const char *url, const char *body,
               rn_http_resp_t *out) {
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);

    url_t u;
    if (!parse_url(url, &u)) {
        ESP_LOGW(TAG, "bad url: %s", url);
        return -2;
    }

#if USE_PROXY
    /* The proxy does the resolving, so its address is all that must work. */
    int fd = tcp_connect(PROXY_HOST, PROXY_PORT);
    if (fd >= 0 && !proxy_tunnel(fd, u.host, u.port)) {
        close(fd);
        fd = -1;
    }
#else
    int fd = tcp_connect(u.host, u.port);
#endif
    if (fd < 0) {
        return -3;
    }

    tls_t tls;
    if (!tls_start(&tls, fd, u.host)) {
        close(fd);
        return -4;
    }

    char req[512];
    const size_t blen = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nx-device-key: %s\r\n"
                     "accept: application/json\r\nuser-agent: freewili-roomnode\r\n"
                     "connection: close\r\n",
                     method, u.path, u.host, DEVICE_KEY);
    if (blen > 0) {
        n += snprintf(req + n, sizeof req - (size_t)n,
                      "content-type: application/json\r\ncontent-length: %u\r\n",
                      (unsigned)blen);
    }
    n += snprintf(req + n, sizeof req - (size_t)n, "\r\n");

    bool sent = tls_write_all(&tls, req, (size_t)n);
    if (sent && blen > 0) {
        sent = tls_write_all(&tls, body, blen);
    }
    if (!sent) {
        ESP_LOGW(TAG, "request write failed");
        tls_stop(&tls);
        close(fd);
        return -5;
    }

    static char raw[1536];
    const int got = tls_read_all(&tls, raw, sizeof raw);
    tls_stop(&tls);
    close(fd);

    if (got <= 0) {
        ESP_LOGW(TAG, "empty response");
        return -6;
    }

    /* Status line. */
    if (strncmp(raw, "HTTP/1.", 7) != 0) {
        ESP_LOGW(TAG, "not http: %.32s", raw);
        return -7;
    }
    const char *sp = strchr(raw, ' ');
    out->status = sp ? atoi(sp + 1) : 0;

    /* The Date header is the only wall clock this node has. */
    const char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start == NULL) {
        ESP_LOGW(TAG, "no header terminator");
        return -8;
    }
    const char *hdr_end = body_start;
    for (const char *line = strstr(raw, "\r\n"); line != NULL && line < hdr_end; ) {
        const char *next = strstr(line + 2, "\r\n");
        if (next == NULL) {
            break;
        }
        if (strncasecmp(line + 2, "Date:", 5) == 0) {
            char val[64];
            const size_t vlen = (size_t)(next - (line + 2)) - 5u;
            snprintf(val, sizeof val, "%.*s", (int)(vlen < sizeof val - 1 ? vlen : sizeof val - 1),
                     line + 7);
            out->server_epoch = parse_http_date(val);
        }
        line = next;
    }

    const bool chunked = strcasestr(raw, "transfer-encoding: chunked") != NULL;
    take_body(raw, body_start + 4, sizeof out->body, chunked, out->body);
    return 0;
}
