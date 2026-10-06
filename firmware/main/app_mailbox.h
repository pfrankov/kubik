// Fixed-size inbox for wake/playback/card events; caller owns the short critical section.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint8_t type; int32_t a, b; uint32_t session; } app_ev_t;
#define APP_MAILBOX_SLOTS 8

typedef struct {
    app_ev_t events[APP_MAILBOX_SLOTS];
    uint32_t order[APP_MAILBOX_SLOTS], next;
    uint8_t pending;
} app_mailbox_t;

void app_mailbox_put(app_mailbox_t *box, unsigned slot, app_ev_t event);
bool app_mailbox_take(app_mailbox_t *box, app_ev_t *event);
