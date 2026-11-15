/* The C6's only job: keep the main CPU's picture of the backend current, and
 * keep the backend's picture of the room current. No policy of its own -- if a
 * poll fails, the last known command stands. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

void rn_api_start(void);

/* Presence edges arrive from the link task. This is the only input the C6
 * reacts to. */
void rn_api_note_presence(bool present);
