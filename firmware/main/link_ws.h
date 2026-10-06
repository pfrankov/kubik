#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "link.h"

typedef struct {
    const char *tag;
    bool *connected, *ready, *failed, *hello_sent, *pairing, *usb_ready;
    uint32_t *down_ms;
    char **rx;
    uint8_t *rx_op;
    atomic_int *via, *problem;
    link_handlers_t *handlers;
    void *active_ws;
    int via_none, via_wifi;
    void (*lock_route)(void);
    void (*unlock_route)(void);
    uint32_t (*now_ms)(void);
    bool (*is_welcome)(const char *text, size_t len);
    void (*handshake)(const char *text, size_t len, bool usb, uint32_t epoch);
    void (*set_via)(int via);
} link_ws_context_t;

void link_ws_bind(link_ws_context_t *context);
void link_ws_dispatch(void *arg, int32_t id, void *data);
