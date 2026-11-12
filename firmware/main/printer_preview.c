#include "printer_preview.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

struct printer_preview {
    atomic_uint refs;
    atomic_bool presented;
    uint32_t session, hash;
    int64_t deadline;
    unsigned offset;
    char rid[33];
    bool active;
    uint8_t kind;
    uint8_t data[PRINTER_PREVIEW_STORAGE];
};
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static printer_preview_t *current;
static atomic_uint allocations;

void printer_preview_retain(printer_preview_t *p) { if (p) atomic_fetch_add(&p->refs, 1); }
void printer_preview_release(printer_preview_t *p) {
    if (p && atomic_fetch_sub(&p->refs, 1) == 1) { free(p); atomic_fetch_sub(&allocations, 1); }
}
const uint8_t *printer_preview_pixels(const printer_preview_t *p) { return p ? p->data : NULL; }
uint32_t printer_preview_hash(const printer_preview_t *p) { return p ? p->hash : 0; }
bool printer_preview_is_screen(const printer_preview_t *p) { return p && p->kind == 1; }
bool printer_preview_draw_allowed(const printer_preview_t *p, bool menu_open, float progress) {
    return p && (!p->kind || (!menu_open && progress <= 0));
}
void printer_preview_presented(printer_preview_t *p) { if (p) atomic_store(&p->presented, true); }
bool printer_preview_was_presented(const printer_preview_t *p) { return p && atomic_load(&p->presented); }

static bool valid_id(const uint8_t *id) {
    for (unsigned i = 0; i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}
static printer_preview_t *create(const uint8_t *id, uint32_t session, int64_t now, uint8_t kind) {
    unsigned expected = atomic_load(&allocations);
    do { if (expected >= 2) return NULL; }
    while (!atomic_compare_exchange_weak(&allocations, &expected, expected + 1));
#if CONFIG_IDF_TARGET_ESP32S3
    unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    printer_preview_t *p = heap_caps_calloc(1, sizeof *p, caps);
    if (!p) { atomic_fetch_sub(&allocations, 1); return NULL; }
    atomic_init(&p->refs, 1); atomic_init(&p->presented, false); p->session = session; p->kind = kind; p->hash = 2166136261u; p->deadline = now + 100000000;
    memcpy(p->rid, id, 32); return p;
}
static bool frame_valid(uint8_t tag, const uint8_t *data, size_t length, uint32_t session) {
    return tag <= 1 && session && data && length >= 35 && length <= 34 + PRINTER_PREVIEW_CHUNK && valid_id(data);
}
static bool frame_bounds(unsigned offset, unsigned count) {
    return offset <= PRINTER_PREVIEW_BYTES && count <= PRINTER_PREVIEW_BYTES - offset;
}
static bool frame_matches(const printer_preview_t *p, const uint8_t *data, uint32_t session, uint8_t kind) {
    return p && p->session == session && p->kind == kind && !memcmp(p->rid, data, 32) && !p->active;
}
#if !CONFIG_IDF_TARGET_ESP32S3
// The wire remains 128x128/16 colors. Retain alternate source columns and
// rows before BLE starts to preserve memory for Wi-Fi and continuous Live.
static void write_pixel(printer_preview_t *p, unsigned source, unsigned color) {
    unsigned x = source % 128, y = source / 128;
    if ((x & 1) || (y & 1)) return;
    unsigned dst = (y / 2) * PRINTER_PREVIEW_EDGE + x / 2;
    unsigned index = 32 + dst / 2;
    if (dst & 1) p->data[index] |= color;
    else p->data[index] = color << 4;
}
#endif
static void write_frame(printer_preview_t *p, const uint8_t *data, unsigned offset, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        unsigned at = offset + i;
        uint8_t value = data[34 + i];
        p->hash = (p->hash ^ value) * 16777619u;
#if CONFIG_IDF_TARGET_ESP32S3
        p->data[at] = value;
#else
        if (at < 32) p->data[at] = value;
        else {
            unsigned source = 2 * (at - 32);
            write_pixel(p, source, value >> 4);
            write_pixel(p, source + 1, value & 15);
        }
#endif
    }
    p->offset += count;
}
static bool protected_preview(const printer_preview_t *p, uint32_t session, int64_t now,
                              const uint8_t *id, uint8_t kind) {
    if (!p || !p->active || p->session != session || now >= p->deadline) return false;
    return !p->kind || (p->kind == kind && !memcmp(p->rid, id, 32));
}
static bool install_fresh(printer_preview_t *fresh, printer_preview_t **retired, uint32_t session,
                          int64_t now, const uint8_t *id, uint8_t tag) {
    if (!fresh) return true;
    if (protected_preview(current, session, now, id, tag)) return false;
    *retired = current; current = fresh;
    return true;
}
void printer_preview_receive(uint8_t tag, const uint8_t *data, size_t length, uint32_t session) {
    if (!frame_valid(tag, data, length, session)) return;
    unsigned offset = ((unsigned)data[32] << 8) | data[33], count = length - 34;
    if (!frame_bounds(offset, count)) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    bool protected = protected_preview(current, session, now, data, tag);
    portEXIT_CRITICAL(&lock);
    if (!offset && protected) return;
    printer_preview_t *fresh = offset ? NULL : create(data, session, now, tag), *retired = NULL;
    if (!offset && !fresh) return;
    portENTER_CRITICAL(&lock);
    if (!install_fresh(fresh, &retired, session, now, data, tag)) {
        portEXIT_CRITICAL(&lock); printer_preview_release(fresh); return;
    }
    printer_preview_t *p = current;
    if (frame_matches(p, data, session, tag)) {
        if (p->offset == offset && now < p->deadline) write_frame(p, data, offset, count);
        else { current = NULL; if (!retired) retired = p; }
    }
    portEXIT_CRITICAL(&lock);
    printer_preview_release(retired);
}
static bool activate(uint32_t session, const char *rid, uint8_t kind) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    bool ok = current && current->session == session && rid && !strcmp(current->rid, rid) &&
        current->kind == kind && current->offset == PRINTER_PREVIEW_BYTES && now < current->deadline;
    if (ok) { current->active = true; if (kind) current->deadline = now + 15000000; }
    portEXIT_CRITICAL(&lock);
    return ok;
}
bool printer_preview_activate(uint32_t session, const char *rid) { return activate(session, rid, 0); }
bool printer_preview_activate_screen(uint32_t session, const char *rid) { return activate(session, rid, 1); }
bool printer_preview_screen_presented(uint32_t session, const char *rid) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    bool shown = current && current->session == session && current->kind == 1 && current->active && rid &&
        !strcmp(current->rid, rid) && now < current->deadline && atomic_load(&current->presented);
    portEXIT_CRITICAL(&lock);
    return shown;
}
void printer_preview_cancel_screen(uint32_t session, const char *rid) {
    portENTER_CRITICAL(&lock);
    printer_preview_t *old = current && current->kind == 1 && current->session == session && rid &&
        !strcmp(current->rid, rid) ? current : NULL;
    if (old) current = NULL;
    portEXIT_CRITICAL(&lock);
    printer_preview_release(old);
}
bool printer_preview_dismiss_screen(uint32_t session) {
    portENTER_CRITICAL(&lock);
    printer_preview_t *old = current && current->kind == 1 && current->session == session ? current : NULL;
    if (old) current = NULL;
    portEXIT_CRITICAL(&lock);
    printer_preview_release(old);
    return old != NULL;
}
void printer_preview_clear_print(uint32_t session) {
    portENTER_CRITICAL(&lock);
    printer_preview_t *old = current && !current->kind && current->session == session ? current : NULL;
    if (old) current = NULL;
    portEXIT_CRITICAL(&lock);
    printer_preview_release(old);
}
void printer_preview_discard(uint32_t session, const char *rid) {
    portENTER_CRITICAL(&lock);
    printer_preview_t *old = current && !current->active && current->session == session && rid &&
        !strcmp(current->rid, rid) ? current : NULL;
    if (old) current = NULL;
    portEXIT_CRITICAL(&lock);
    printer_preview_release(old);
}
void printer_preview_clear(uint32_t session) {
    portENTER_CRITICAL(&lock);
    printer_preview_t *old = current && (!session || current->session == session) ? current : NULL;
    if (old) current = NULL;
    portEXIT_CRITICAL(&lock);
    printer_preview_release(old);
}
printer_preview_t *printer_preview_snapshot(uint32_t session) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    printer_preview_t *old = current && (current->session != session || now >= current->deadline) ? current : NULL;
    if (old) current = NULL;
    printer_preview_t *p = current && current->active ? current : NULL;
    printer_preview_retain(p);
    portEXIT_CRITICAL(&lock);
    printer_preview_release(old);
    return p;
}
