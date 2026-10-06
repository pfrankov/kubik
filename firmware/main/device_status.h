#pragma once
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    bool open, connected, radio, agent, dragged;
    unsigned page;
    int drag_x, drag_y;
    char ssid[33], ip[16], gateway[16], dns[16], mac[18], hostname[32], name[32], firmware[32], route[8], url[160];
} device_status_t;

static inline bool device_status_back(device_status_t *s) {
    if (!s->open) return false;
    s->open = false; return true;
}
static inline bool device_status_tap(device_status_t *s, int y) {
    if (!s->open) return false;
    if (y >= 424) s->page = (s->page + 1) % 3;
    return true;
}
static inline bool device_status_drag(device_status_t *s, int x, int y, bool first) {
    if (!s->open) return false;
    if (first) { s->drag_x = x; s->drag_y = y; s->dragged = false; }
    int dx = x - s->drag_x, dy = y - s->drag_y;
    if (!s->dragged && (abs(dx) >= 40 || abs(dy) >= 40)) {
        s->page = (s->page + ((abs(dx) > abs(dy) ? dx : dy) < 0 ? 1 : 2)) % 3;
        s->dragged = true;
    }
    return true;
}
