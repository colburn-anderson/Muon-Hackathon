/* Wi-Fi station bring-up for the room node.
 *
 * Deliberately small: connect, retry forever, and expose a flag. The API task
 * does not gate on it -- a failed poll just reports "unreachable" -- because a
 * device that stops polling the moment the flag clears never recovers from the
 * flag being wrong. */

#pragma once

#include <stdbool.h>

void rn_wifi_start(void);
bool rn_wifi_up(void);
