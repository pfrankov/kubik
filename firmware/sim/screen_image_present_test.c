#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef struct { bool screen, presented; } printer_preview_t;
static int order, waits, marks;
static bool printer_preview_is_screen(const printer_preview_t *p) { return p->screen; }
static bool printer_preview_was_presented(const printer_preview_t *p) { return p->presented; }
static void display_wait_idle(void) { assert(order == 0); order = 1; waits++; }
static void printer_preview_presented(printer_preview_t *p) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (p->screen && !p->presented) assert(order == 1);
#endif
    order = 2; marks++; p->presented = true;
}
/* PRODUCTION_PRESENT_RECEIPT */
int main(void) {
    printer_preview_t image = {.screen = true}, print = {0};
    note_preview_presented(false, &image); assert(!waits && !marks);
    note_preview_presented(true, NULL); assert(!waits && !marks);
    note_preview_presented(true, &image);
#if CONFIG_IDF_TARGET_ESP32S3
    assert(waits == 1);
#else
    assert(!waits);
#endif
    assert(marks == 1); order = 0;
    note_preview_presented(true, &image); assert(marks == 2);
    order = 0; note_preview_presented(true, &print); assert(marks == 3);
#if CONFIG_IDF_TARGET_ESP32S3
    assert(waits == 1);
#else
    assert(!waits);
#endif
    puts("screen image: only first S3 screen receipt waits for DMA completion");
}
