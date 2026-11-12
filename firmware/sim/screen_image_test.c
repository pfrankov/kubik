#include "app.h"
#include "printer_preview.h"
#include "screen_image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t now;
static uint32_t session = 1;
static char sent[256];
static int posts;
static bool send_ok = true;
int64_t esp_timer_get_time(void) { return now; }
void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void)caps; return calloc(n, size); }
uint32_t link_session(void) { return session; }
void app_post_in_session(app_ev_type_t type, int a, int b, uint32_t expected) {
    (void)a; (void)b; assert(type == EV_SRV_PREVIEW && expected == session); posts++;
}
bool link_send_json_in_session(const char *json, uint32_t expected) {
    assert(expected == session); if (!send_ok) return false; snprintf(sent, sizeof sent, "%s", json); return true;
}
bool agent_protocol_has_nul_escape(const char *raw, size_t len) { (void)len; return strstr(raw, "\\u0000") != NULL; }
const char *printer_parse_id(const cJSON *request) {
    const char *rid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "rid"));
    if (!rid || strlen(rid) != 32) return NULL;
    for (unsigned i = 0; i < 32; i++) if (!((rid[i] >= '0' && rid[i] <= '9') || (rid[i] >= 'a' && rid[i] <= 'f'))) return NULL;
    return rid;
}
static const char rid[] = "0123456789abcdef0123456789abcdef";
static void transfer(uint8_t kind) {
    uint8_t frame[34 + PRINTER_PREVIEW_CHUNK] = {0}; memcpy(frame, rid, 32);
    for (unsigned off = 0; off < PRINTER_PREVIEW_BYTES;) {
        unsigned n = PRINTER_PREVIEW_BYTES - off;
        if (n > PRINTER_PREVIEW_CHUNK) n = PRINTER_PREVIEW_CHUNK;
        frame[32] = off >> 8; frame[33] = off;
        printer_preview_receive(kind, frame, 34 + n, session); off += n;
    }
}
static void receive(const char *action) {
    char json[128]; snprintf(json, sizeof json, "{\"t\":\"screen_image\",\"action\":\"%s\",\"rid\":\"%s\"}", action, rid);
    cJSON *request = cJSON_Parse(json); assert(request);
    assert(screen_image_receive(request, json, strlen(json), strlen(json), session)); cJSON_Delete(request);
}
int main(void) {
    transfer(1); receive("show"); assert(posts == 1); screen_image_poll(); assert(!sent[0]);
    printer_preview_t *image = printer_preview_snapshot(session); assert(image);
    printer_preview_presented(image); screen_image_poll(); assert(strstr(sent, "\"status\":\"shown\""));
    sent[0] = 0; printer_preview_release(image);
    receive("cancel"); assert(!printer_preview_snapshot(session));
    transfer(1); receive("show"); receive("cancel"); screen_image_poll(); assert(!sent[0]);
    transfer(1); receive("show"); now += 4500001; screen_image_poll();
    assert(strstr(sent, "\"code\":\"timeout\"")); assert(!printer_preview_snapshot(session));
    sent[0] = 0; transfer(0); assert(printer_preview_activate(session, rid));
    transfer(1); receive("show"); screen_image_poll(); assert(strstr(sent, "\"code\":\"unavailable\""));
    image = printer_preview_snapshot(session); assert(image); printer_preview_release(image);
    printer_preview_clear(session); sent[0] = 0;
    transfer(1); receive("show"); session = 2; screen_image_poll(); assert(!sent[0]);
    printer_preview_clear(0); receive("show"); screen_image_poll();
    assert(strstr(sent, "\"code\":\"unavailable\"")); sent[0] = 0;
    transfer(1); receive("show"); printer_preview_t *last = printer_preview_snapshot(session); assert(last);
    printer_preview_presented(last); printer_preview_release(last); screen_image_poll();
    assert(strstr(sent, "\"status\":\"shown\""));
    sent[0] = 0; receive("cancel"); transfer(1); receive("show");
    last = printer_preview_snapshot(session); assert(last); printer_preview_presented(last); printer_preview_release(last);
    send_ok = false; screen_image_poll(); now += 5500001; screen_image_poll();
    assert(!printer_preview_snapshot(session)); send_ok = true;
    transfer(1); receive("show"); last = printer_preview_snapshot(session); assert(last);
    printer_preview_presented(last); printer_preview_release(last); screen_image_poll();
    assert(strstr(sent, "\"status\":\"shown\""));
    puts("screen image: session-bound presentation ACK, cancel, timeout and printer isolation passed");
}
