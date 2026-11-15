/* Every tunable the C6 room node has. One header, no menuconfig hunting, so a
 * reflash to change a network is a one-line edit.
 *
 * Placeholders marked PLACEHOLDER are expected to be replaced by the real
 * values from the AWS/backend team. Nothing else here should need to change
 * when they land, as long as the URL shapes hold. */

#pragma once

/* ---- Secrets --------------------------------------------------------
 * Credentials and tokens are NOT in this file: they live in
 * roomnode_secrets.h, which is git-ignored (copy the .example). A fresh
 * checkout builds with placeholders and simply fails to associate, which is
 * the correct behaviour for a file that should never be committed. */
#if defined(__has_include)
#  if __has_include("roomnode_secrets.h")
#    include "roomnode_secrets.h"
#  endif
#endif

#ifndef WIFI_SSID
#  define WIFI_SSID     "unset-copy-roomnode_secrets.h.example"
#  define WIFI_PASSWORD ""
#endif

/* ---- Wi-Fi station -------------------------------------------------- */
/* WIFI_SSID / WIFI_PASSWORD come from roomnode_secrets.h above. */

/* ---- Bench HTTP proxy ----------------------------------------------
 * The C6 is the only thing on the tether's 192.168.49.x network, and the only
 * route off it is this proxy -- so every call is CONNECT-tunnelled. net.c does
 * the tunnel by hand; neither esp_http_client nor esp-tls can be told about a
 * proxy.
 *
 * It also resolves names for us, which is load-bearing: the tether hands out no
 * working resolver, so a direct connection cannot even find the gateway.
 *
 * Set USE_PROXY 0 on a normal network with direct egress. */
#define USE_PROXY       1
#define PROXY_HOST      "192.168.49.1"
#define PROXY_PORT      8228

/* ---- API ------------------------------------------------------------
 * The real gateway (Room Lock API, Nov 2026), HTTPS only.
 *
 * Device auth is the x-device-key header, NOT a Cognito token: this box is a
 * fixture, not a user. The key comes from roomnode_secrets.h. */
#define API_BASE        "https://6526ig971l.execute-api.us-east-2.amazonaws.com"
#define ROOM_ID         "1"

/* Device credential, sent as x-device-key. Committed on purpose: it is the
 * shared dev key for this build, and a key that has to be copied by hand turns
 * a fresh checkout into a silent 403 that looks like a network fault. For a
 * production key, define it in roomnode_secrets.h instead -- this default is
 * guarded, so the secrets value wins. */
#ifndef DEVICE_KEY
#  define DEVICE_KEY "testkey"
#endif

/* The QR destination comes only from the room GET response's `url` field. */

/* ---- Timing ---------------------------------------------------------
 * The guide asks for a GET every 5 s; this runs at 1 s because that poll is
 * also how servo and alert commands arrive, so it is the latency of every other
 * behaviour here, and 1 request/s is well inside what the endpoint tolerates. */
#define POLL_MS         1000
#define WIFI_RETRY_MS   5000

/* Fail secure independently of display state: after this long without a
 * successful room GET, command the door locked. */
#define OFFLINE_LOCK_MS 60000

/* Presence is re-posted on every change AND this often, so one dropped POST
 * cannot make the server conclude the room is empty. The guide says 60 s; 5 s
 * costs one small POST per poll and makes a lost one invisible. */
#define PRESENCE_RESEND_MS  5000

/* Presence debounce and absence hold are enforced by the main RP2040; the C6
 * receives only the resulting boolean belief. */

/* ---- Link to the main CPU -------------------------------------------
 * UART0 on the C6's GPIO16 (TX) / GPIO17 (RX) is the wire into the main CPU's
 * breakout UART1: main GPIO9 RX <- 16, main GPIO8 TX -> 17. 115200 baud 8N1.
 * The control traffic is only a few lines every five seconds; this rate gives
 * main's 32-byte UART FIFO enough service margin without an IRQ-driven queue.
 *
 * Hardware flow control is deliberately OFF on this side: the main CPU pulls
 * its CTS input up, so it always has clear-to-send, and our lines are tiny at
 * a low rate. Turning it on without the RTS wire present is how a link
 * deadlocks. */
#define LINK_UART_NUM   0
#define LINK_TX_GPIO    16
#define LINK_RX_GPIO    17
#define LINK_BAUD       115200
