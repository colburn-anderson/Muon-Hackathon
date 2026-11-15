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

/* Queue an "I'm done" request. The API task performs the POST so HTTP calls
 * remain serialized even though the request originates in the link task. */
void rn_api_end(void);
/* The main CPU announced a boot: forget what we had already sent it. */
void rn_api_note_main_boot(void);

/* USB-console test override. SECONDS supplies a relative expiry for held,
 * reserved and warn states; zero means no countdown. */
bool rn_api_mock_state(const char *state, int seconds);
void rn_api_mock_off(void);
