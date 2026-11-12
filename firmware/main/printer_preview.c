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
void printer_preview_presented(printer_preview_t *p) { if (p) atomic_store(&p->presented, true); }
bool printer_preview_was_presented(const printer_preview_t *p) { return p && atomic_load(&p->presented); }

static bool valid_id(const uint8_t *id) {
    for (unsigned i = 0; i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}
static printer_preview_t *create(const uint8_t *id, uint32_t session, int64_t now) {
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
    atomic_init(&p->refs, 1); atomic_init(&p->presented, false); p->session = session; p->hash = 2166136261u; p->deadline = now + 100000000;
    memcpy(p->rid, id, 32); return p;
}
static bool frame_valid(uint8_t tag, const uint8_t *data, size_t length, uint32_t session) {
    return !tag && session && data && length >= 35 && length <= 34 + PRINTER_PREVIEW_CHUNK && valid_id(data);
}
static bool frame_bounds(unsigned offset, unsigned count) {
    return offset <= PRINTER_PREVIEW_BYTES && count <= PRINTER_PREVIEW_BYTES - offset;
}
static bool active_session(uint32_t session, int64_t now) {
    portENTER_CRITICAL(&lock);
    bool active = current && current->active && current->session == session && now < current->deadline;
    portEXIT_CRITICAL(&lock);
    return active;
}
static bool frame_matches(const printer_preview_t *p, const uint8_t *data, uint32_t session) {
    return p && p->session == session && !memcmp(p->rid, data, 32) && !p->active;
}
#if !CONFIG_IDF_TARGET_ESP32S3
// The wire remains 128x128/16 colors. Retain three of each four source
// columns and rows before BLE starts, halving the C6 framebuffer footprint.
static void write_pixel(printer_preview_t *p, unsigned source, unsigned color) {
    unsigned x = source % 128, y = source / 128;
    if (x % 4 == 3 || y % 4 == 3) return;
    unsigned dst = ((y / 4) * 3 + y % 4) * 96 + (x / 4) * 3 + x % 4;
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
void printer_preview_receive(uint8_t tag, const uint8_t *data, size_t length, uint32_t session) {
    if (!frame_valid(tag, data, length, session)) return;
    unsigned offset = ((unsigned)data[32] << 8) | data[33], count = length - 34;
    if (!frame_bounds(offset, count)) return;
    int64_t now = esp_timer_get_time();
    if (!offset && active_session(session, now)) return;
    printer_preview_t *fresh = offset ? NULL : create(data, session, now), *retired = NULL;
    if (!offset && !fresh) return;
    portENTER_CRITICAL(&lock);
    if (fresh) { retired = current; current = fresh; }
    printer_preview_t *p = current;
    if (frame_matches(p, data, session)) {
        if (p->offset == offset && now < p->deadline) write_frame(p, data, offset, count);
        else { current = NULL; if (!retired) retired = p; }
    }
    portEXIT_CRITICAL(&lock);
    printer_preview_release(retired);
}
bool printer_preview_activate(uint32_t session, const char *rid) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    bool ok = current && current->session == session && rid && !strcmp(current->rid, rid) &&
        current->offset == PRINTER_PREVIEW_BYTES && now < current->deadline;
    if (ok) current->active = true;
    portEXIT_CRITICAL(&lock);
    return ok;
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
