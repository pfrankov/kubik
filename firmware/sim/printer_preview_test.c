#include "printer_preview.h"
#include "render_printer_preview.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
bool g_render_swap;
static int64_t now;
int64_t esp_timer_get_time(void) { return now; }
void *heap_caps_calloc(size_t n, size_t s, unsigned caps) { (void)caps; return calloc(n, s); }
static const char rid[] = "0123456789abcdef0123456789abcdef";
static uint8_t frame[34 + PRINTER_PREVIEW_CHUNK];
static void chunk(unsigned offset, unsigned count, uint32_t session) {
    memcpy(frame, rid, 32); frame[32] = offset >> 8; frame[33] = offset;
    for (unsigned i = 0; i < count; i++) frame[34 + i] = (uint8_t)(offset + i);
    printer_preview_receive(0, frame, 34 + count, session);
}
static void transfer(uint32_t session) {
    for (unsigned offset = 0; offset < PRINTER_PREVIEW_BYTES;) {
        unsigned n = PRINTER_PREVIEW_BYTES - offset;
        if (n > PRINTER_PREVIEW_CHUNK) n = PRINTER_PREVIEW_CHUNK;
        chunk(offset, n, session); offset += n;
    }
}
int main(void) {
    chunk(0, 50, 1); assert(!printer_preview_activate(1, rid));
    printer_preview_discard(2, rid); printer_preview_discard(1, "wrong");
    chunk(50, 50, 1); printer_preview_discard(1, rid);
    chunk(100, 50, 1); // Late chunk after abort cannot resurrect the discarded transfer.
    assert(!printer_preview_activate(1, rid));
    chunk(0, 50, 1);
    chunk(55, 50, 1); assert(!printer_preview_activate(1, rid));
    transfer(1); assert(!printer_preview_activate(2, rid)); assert(printer_preview_activate(1, rid));
    printer_preview_discard(1, rid); // Refused unrelated admission cannot discard an active image.
    printer_preview_t *a = printer_preview_snapshot(1); assert(a && printer_preview_pixels(a)[100] == 100);
    assert(!printer_preview_was_presented(a)); printer_preview_presented(a);
    assert(printer_preview_was_presented(a));
    scene_t scene = {0}; scene.printer_preview = a;
    prim_t prim = {.bx0 = 0, .by0 = 0, .bx1 = 127, .by1 = 127};
    uint16_t row[128] = {0};
    render_printer_preview(&scene, &prim, row, 0, 128, 0, 0, 0);
    assert(row[0] == 0x0405 && row[1] == 0x0001); // first index pair is 0x20
    g_render_swap = true;
    render_printer_preview(&scene, &prim, row, 0, 128, 0, 0, 0);
    assert(row[0] == 0x0504 && row[1] == 0x0100);
    uint32_t hash = printer_preview_hash(a); chunk(0, 80, 1);
    printer_preview_t *same = printer_preview_snapshot(1); assert(same == a && printer_preview_hash(same) == hash);
    printer_preview_release(same); printer_preview_clear(1);
    assert(!printer_preview_snapshot(1)); assert(printer_preview_pixels(a)[100] == 100);
    transfer(2); assert(printer_preview_activate(2, rid));
    assert(!printer_preview_snapshot(1)); assert(printer_preview_pixels(a)[100] == 100);
    printer_preview_release(a);
    transfer(3); assert(printer_preview_activate(3, rid));
    now += 100000001; assert(!printer_preview_snapshot(3));
    puts("printer preview: ordered bounded frames, session, immutable snapshots, timeout passed");
}
