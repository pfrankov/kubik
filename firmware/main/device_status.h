#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include "ui_kit.h"

typedef struct {
    bool open, connected, radio, agent, dragged;
    bool wifi_setup;  // the Wi-Fi button was tapped; the app consumes it
    unsigned page;
    float enter;  // seconds since the page last changed
    int drag_x, drag_y;
    char ssid[33], ip[16], gateway[16], dns[16], mac[18], hostname[32], name[32], firmware[32], route[8], url[160];
} device_status_t;

static inline bool device_status_back(device_status_t *s) {
    if (!s->open) return false;
    s->open = false; return true;
}
// The Wi-Fi setup button on the Network page's hero card.
#define STATUS_WIFI_X 376
#define STATUS_WIFI_Y 135
#define STATUS_WIFI_HW 64
#define STATUS_WIFI_HH 28
static inline bool device_status_wifi_hit(int x, int y) {
    return abs(x - STATUS_WIFI_X) <= STATUS_WIFI_HW + 6 && abs(y - STATUS_WIFI_Y) <= STATUS_WIFI_HH + 12;
}
static inline void device_status_page(device_status_t *s, unsigned page) {
    if (s->page != page) s->enter = 0;
    s->page = page;
}
static inline bool device_status_tap(device_status_t *s, int x, int y) {
    if (!s->open) return false;
    if (s->page == 0 && device_status_wifi_hit(x, y)) { s->wifi_setup = true; return true; }
    int hit = ui_segment_hit(3, x, y, 428);
    if (hit >= 0) device_status_page(s, (unsigned)hit);
    else if (y >= 396) device_status_page(s, (s->page + 1) % 3);
    return true;
}
static inline bool device_status_drag(device_status_t *s, int x, int y, bool first) {
    if (!s->open) return false;
    if (first) { s->drag_x = x; s->drag_y = y; s->dragged = false; }
    int dx = x - s->drag_x, dy = y - s->drag_y;
    if (!s->dragged && (abs(dx) >= 40 || abs(dy) >= 40)) {
        device_status_page(s, (s->page + ((abs(dx) > abs(dy) ? dx : dy) < 0 ? 1 : 2)) % 3);
        s->dragged = true;
    }
    return true;
}
