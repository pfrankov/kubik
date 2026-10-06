// Pokes: a tap, a pet, a shake or a pick-up tells the server that its owner is playing with Kubik. The server only
// logs it, so a poke is never urgent: the touching code (the app task, above the display) just notes it, and the
// link task (below the display) sends at most one a second, the most telling of those noted meanwhile.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum { POKE_TAP, POKE_PET, POKE_SHAKE, POKE_PICKUP } poke_kind_t;

void link_poke(poke_kind_t kind);  // any task: a flag, nothing is sent
// The link task, once a tick: sends the noted poke if a second has passed since the last one.
void link_poke_tick(uint32_t now_ms);
