/* Things both halves of the room node need to agree on that are not protocol.
 *
 * The QR URL is compiled in on the main CPU and rides to the display in the
 * state frame. It is the same string the C6 carries as QR_URL; when the web app
 * moves, both constants move, and the display picks it up without a reflash of
 * the display half -- the main image carries it. */

#ifndef FWOG_ROOM_CONFIG_H
#define FWOG_ROOM_CONFIG_H

#define ROOM_ID_STR  "1"
#define ROOM_QR_URL  "https://main.d24j7edplzktpl.amplifyapp.com/r/1"

#endif
