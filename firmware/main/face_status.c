#include "face_internal.h"
#include "ui_kit.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>

// Cards share the Settings/Agent geometry: 20 px margin, 12 px gaps, UI_RADIUS corners.
//   header (y 42) | hero card (y 80..190) | detail card (y 202..394) | pager (y 428)
#define HERO_Y 135.f
#define DETAIL_Y 298.f
#define ROW_X 44.f
#define ROW_RIGHT 436.f
#define TEXT_X 100.f
#define PAGER_Y 428.f

static void card(scene_t *s, float cy, float hh) { sc_rbox(s, 240, cy, 220, hh, UI_RADIUS, 0, UI_SURFACE, 1); }
static void fitted(scene_t *s, text_role_t role, const char *text, float x, float y, int align, float width, uint32_t color) {
    char line[168];
    ui_fit(line, sizeof line, text[0] ? text : "Unavailable", role, width);
    sc_label(s, role, line, x, y, align, color, 1);
}
static void hero(scene_t *s, int icon, uint32_t icon_color, const char *primary, uint32_t primary_color,
                 const char *secondary, float width) {
    card(s, HERO_Y, 55);
    sc_icon(s, icon, 68, HERO_Y, 40, icon_color, 1);
    fitted(s, TXT_PAGE_BODY, primary, TEXT_X, HERO_Y - 14, -1, width, primary_color);
    fitted(s, TXT_CAPTION, secondary, TEXT_X, HERO_Y + 22, -1, width, UI_MUTED);
}
// Wi-Fi setup belongs with the network it changes.
static void wifi_button(scene_t *s) {
    sc_rbox(s, STATUS_WIFI_X, STATUS_WIFI_Y, STATUS_WIFI_HW, STATUS_WIFI_HH, STATUS_WIFI_HH, 0, UI_PRESSED, 1);
    float left = STATUS_WIFI_X - 40;
    sc_icon(s, ICON_QR_CODE, left, STATUS_WIFI_Y, 26, UI_ACCENT, 1);
    sc_label(s, TXT_ACTION, "Wi-Fi", left + 22, STATUS_WIFI_Y, -1, UI_TEXT, 1);
}
static void row(scene_t *s, const char *label, const char *value, float y, bool muted) {
    sc_label(s, TXT_CAPTION, label, ROW_X, y, -1, UI_MUTED, 1);
    float room = ROW_RIGHT - ROW_X - label_width(TXT_CAPTION, label) - 16;
    fitted(s, TXT_ACTION, value, ROW_RIGHT, y, 1, room, muted ? UI_MUTED : UI_TEXT);
}
static void rows(scene_t *s, const char *const *labels, const char *const *values, const bool *muted) {
    card(s, DETAIL_Y, 96);
    for (int i = 0; i < 3; i++) {
        float y = 234 + i * 64;
        row(s, labels[i], values[i], y, muted && muted[i]);
        if (i) sc_rbox(s, 240, y - 32, 192, .75f, .75f, 0, UI_PRESSED, 1);
    }
}
static void network(const device_status_t *st, scene_t *s) {
    bool up = st->connected;
    hero(s, up ? ICON_GLOBE : ICON_WIFI_OFF, up ? UI_ACCENT : st->radio ? UI_ERROR : UI_MUTED,
         st->ssid[0] ? st->ssid : "No saved network", UI_TEXT, up ? "Wi-Fi network" : "Saved network", 190);
    wifi_button(s);
    static const char *const labels[] = {"IP address", "Gateway", "DNS"};
    const char *values[] = {up ? st->ip : "Not connected", up ? st->gateway : "Not connected", up ? st->dns : "Not connected"};
    const bool muted[] = {!up, !up, !up};
    rows(s, labels, values, muted);
}
static void device(const device_status_t *st, scene_t *s) {
    char fw[48]; snprintf(fw, sizeof fw, "Firmware %s", st->firmware);
    hero(s, ICON_BLOCKS, UI_GOLD, st->name, UI_TEXT, fw, 336);
    static const char *const labels[] = {"Processor", "DHCP name", "Wi-Fi MAC"};
    const char *values[] = {"ESP32-C6", st->hostname, st->mac};
    rows(s, labels, values, NULL);
}
static void agent(const device_status_t *st, scene_t *s) {
    bool wifi = !strcmp(st->route, "wifi"), usb = !strcmp(st->route, "usb");
    hero(s, st->agent ? ICON_BRAIN : ICON_UNPLUG, st->agent ? UI_ACCENT : UI_ERROR,
         st->agent ? "Connected" : "Not connected", UI_TEXT,
         wifi ? "Server over Wi-Fi" : usb ? "Server over USB" : "No server link", 336);
    card(s, DETAIL_Y, 96);
    sc_label(s, TXT_CAPTION, "Server address", ROW_X, 224, -1, UI_MUTED, 1);
    const char *at = st->url[0] ? st->url : "Automatic LAN discovery";
    for (int i = 0; i < 4 && *at; i++) {
        float y = 256 + i * 26;
        size_t n = label_prefix(TXT_CAPTION, at, 159, ROW_RIGHT - ROW_X, "");
        if (!n || (i == 3 && at[n])) { fitted(s, TXT_CAPTION, at, ROW_X, y, -1, ROW_RIGHT - ROW_X, UI_TEXT); break; }
        char line[160]; memcpy(line, at, n); line[n] = 0;
        sc_label(s, TXT_CAPTION, line, ROW_X, y, -1, UI_TEXT, 1);
        at += n;
    }
}
void draw_status(const device_status_t *st, scene_t *s) {
    unsigned page = st->page % 3;
    const char *chip = NULL; uint32_t color = UI_ACCENT;
    if (page == 0) {
        chip = st->connected ? "Connected" : st->radio ? "Not connected" : "Radio sleeping";
        color = st->connected ? UI_ACCENT : st->radio ? UI_ERROR : UI_MUTED;
    } else if (page == 2) {
        chip = st->agent ? "Connected" : "Not connected";
        color = st->agent ? UI_ACCENT : UI_ERROR;
    }
    ui_header(s, "Status", chip, color);
    int first = s->n;
    (page == 0 ? network : page == 1 ? device : agent)(st, s);
    static const char *const pages[] = {"Network", "Device", "Agent"};
    ui_segments(s, PAGER_Y, pages, 3, page);
    ui_enter(s, first, st->enter);
}
