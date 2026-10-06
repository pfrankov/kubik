#include "link_poke.h"

#include <stdatomic.h>
#include <stdio.h>

#include "link.h"

#define POKE_MIN_MS 1000  // between two pokes on the wire

static const char *const k_poke_names[] = {[POKE_TAP] = "tap", [POKE_PET] = "pet", [POKE_SHAKE] = "shake", [POKE_PICKUP] = "pickup"};
static atomic_uint s_noted;  // a bit per kind
static uint32_t s_sent_ms;

void link_poke(poke_kind_t kind) { atomic_fetch_or(&s_noted, 1u << kind); }

void link_poke_tick(uint32_t now_ms) {
    if (!atomic_load(&s_noted) || (uint32_t)(now_ms - s_sent_ms) < POKE_MIN_MS) return;
    unsigned noted = atomic_exchange(&s_noted, 0);
    int kind = 31 - __builtin_clz(noted);  // the highest kind is the most telling
    char json[40];
    snprintf(json, sizeof json, "{\"t\":\"poke\",\"kind\":\"%s\"}", k_poke_names[kind]);
    s_sent_ms = now_ms;
    link_send_json(json);
}
