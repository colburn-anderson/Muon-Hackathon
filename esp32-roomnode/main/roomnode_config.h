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
#  define API_TOKEN     "PLACEHOLDER-COGNITO-ID-TOKEN"
#endif

/* ---- Wi-Fi station -------------------------------------------------- */
/* WIFI_SSID / WIFI_PASSWORD come from roomnode_secrets.h above. */

/* ---- Bench HTTP proxy ----------------------------------------------
 * The bench network has no direct route out; everything goes through this
 * proxy as an absolute-URI request. Set USE_PROXY 0 on a normal network.
 *
 * Only plain HTTP is proxied this way. An https:// API_BASE through the proxy
 * needs a CONNECT tunnel, which this client does not implement -- it says so
 * and fails loudly rather than silently hanging. */
#define USE_PROXY       1
#define PROXY_HOST      "192.168.49.1"
#define PROXY_PORT      8228

/* ---- API ------------------------------------------------------------
 * PLACEHOLDER base URL. Deliberately http:// so the proxy path above works
 * today; switch to https:// (and add the tunnel) once the real gateway URL
 * exists. */
#define API_BASE        "http://api.example.invalid/v1"
/* API_TOKEN comes from roomnode_secrets.h. */
#define ROOM_ID         "1"

/* The reservation link this room's QR points at. Not the API. */
#define QR_URL          "https://main.d24j7edplzktpl.amplifyapp.com/r/1"

/* ---- Timing ---------------------------------------------------------
 * POLL_MS is the device's heartbeat: one GET /rooms/1 per period, which is
 * also how servo/alert commands arrive. 2 s is well inside anything AWS
 * charges for and far faster than a door needs. */
#define POLL_MS         2000
#define WIFI_RETRY_MS   5000

/* Presence must persist this long before it counts, and stay away this long
 * before the room is reported empty. Both are the main CPU's job; the C6 only
 * forwards what it is told. Kept here so the number is visible in one place. */
#define PRESENT_DEBOUNCE_S  2
#define ABSENT_TIMEOUT_S    30

/* ---- Link to the main CPU -------------------------------------------
 * UART0 on the C6's GPIO16 (TX) / GPIO17 (RX) is the wire into the main CPU's
 * breakout UART1: main GPIO9 RX <- 16, main GPIO8 TX -> 17. 3 Mbaud 8N1, the
 * rate apps/espuart established.
 *
 * Hardware flow control is deliberately OFF on this side: the main CPU pulls
 * its CTS input up, so it always has clear-to-send, and our lines are tiny at
 * a low rate. Turning it on without the RTS wire present is how a link
 * deadlocks. */
#define LINK_UART_NUM   0
#define LINK_TX_GPIO    16
#define LINK_RX_GPIO    17
#define LINK_BAUD       3000000
