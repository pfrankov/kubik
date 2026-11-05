#include "display_s3.h"
#include "display_s3_tx.h"
#include "viewport.h"
#include "board.h"
#include "frame_pacing_s3.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cpu.h"

#define BUFFERS 3
#define DAMAGE_BANDS (DISPLAY_S3_HEIGHT / DISPLAY_S3_ROWS)
_Static_assert(LCD_PIPE == BUFFERS, "Native staging shares raw DMA buffer ownership");
_Static_assert(UI_RENDER_W >= DISPLAY_S3_WIDTH && UI_RENDER_H == DISPLAY_S3_HEIGHT,
               "Only the renderer's right-hand stripe padding is clipped");
static uint16_t *s_native, *s_dma[BUFFERS];
static uint16_t s_left[DAMAGE_BANDS], s_right[DAMAGE_BANDS];
static int s_x0, s_x1, s_y0, s_y1, s_y;
static bool s_open;
static int64_t s_render_start;
static uint32_t s_render_cycles;
static display_s3_profile_t s_profile;

static void release_buffers(void) {
    heap_caps_free(s_native); s_native = NULL;
    for (int i = 0; i < BUFFERS; i++) { heap_caps_free(s_dma[i]); s_dma[i] = NULL; }
}

bool display_s3_init(void) {
    assert(!s_open);
    display_s3_tx_wait_idle();
    if (!s_native) s_native = heap_caps_calloc(UI_RENDER_W * DISPLAY_S3_HEIGHT,
                                               sizeof(*s_native), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_native) return false;
    for (int i = 0; i < BUFFERS; i++) {
        if (!s_dma[i]) s_dma[i] = heap_caps_malloc(DISPLAY_S3_WIDTH * DISPLAY_S3_ROWS * sizeof(*s_dma[i]),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_dma[i]) { release_buffers(); return false; }
    }
    if (!display_s3_tx_init(s_native, s_dma)) { release_buffers(); return false; }
    memset(s_native, 0, UI_RENDER_W * DISPLAY_S3_HEIGHT * sizeof(*s_native));
    display_s3_begin_frame();
    return true;
}

void display_s3_begin(int x0, int y0, int x1, int y1) {
    assert(s_native && !s_open && x0 >= 0 && y0 >= 0 && x1 <= UI_RENDER_W &&
           y1 <= DISPLAY_S3_HEIGHT && x0 < x1 && y0 < y1);
    s_x0 = x0; s_x1 = x1; s_y0 = s_y = y0; s_y1 = y1; s_open = true;
    s_render_start = esp_timer_get_time();
    s_render_cycles = esp_cpu_get_cycle_count();
}

uint16_t *display_s3_next_row(void) {
    assert(s_open && s_y < s_y1);
    display_s3_tx_wait_writable(s_y);
    int end = s_x1 < DISPLAY_S3_WIDTH ? s_x1 : DISPLAY_S3_WIDTH;
    int visible = end > s_x0 ? end - s_x0 : 0;
    s_profile.input_pixels += (uint32_t)(s_x1 - s_x0);
    s_profile.native_pixels += (uint32_t)visible;
    return s_native + s_y++ * UI_RENDER_W + s_x0;
}

void display_s3_begin_frame(void) {
    assert(!s_open);
    memset(&s_profile, 0, sizeof s_profile);
    for (int band = 0; band < DAMAGE_BANDS; band++) { s_left[band] = DISPLAY_S3_WIDTH; s_right[band] = 0; }
}

void display_s3_end(void) {
    assert(s_open && s_y == s_y1);
    s_open = false;
    s_profile.render_us += esp_timer_get_time() - s_render_start;
    s_profile.render_cycles += (uint32_t)(esp_cpu_get_cycle_count() - s_render_cycles);
    int x0 = s_x0, x1 = s_x1 < DISPLAY_S3_WIDTH ? s_x1 : DISPLAY_S3_WIDTH;
    if (x0 >= x1) return;
    x0 &= ~15; x1 = (x1 + 15) & ~15;
    if (x1 > DISPLAY_S3_WIDTH) x1 = DISPLAY_S3_WIDTH;
    for (int band = s_y0 / DISPLAY_S3_ROWS; band < (s_y1 + DISPLAY_S3_ROWS - 1) / DISPLAY_S3_ROWS; band++) {
        if (x0 < s_left[band]) s_left[band] = (uint16_t)x0;
        if (x1 > s_right[band]) s_right[band] = (uint16_t)x1;
    }
}

static bool has_damage(void) {
    for (int band = 0; band < DAMAGE_BANDS; band++) if (s_left[band] < s_right[band]) return true;
    return false;
}

int64_t display_s3_present(void) {
    assert(!s_open);
    if (!has_damage()) return 0;
    // New CPU work may overlap the old burst; the UI alone consumes fresh TE.
    display_s3_tx_wait_idle();
    int64_t phase = esp_timer_get_time();
    ESP_ERROR_CHECK(frame_pacing_s3_sync());
    s_profile.te_us = esp_timer_get_time() - phase;
    int64_t started = display_s3_tx_submit(s_left, s_right, &s_profile);
    display_s3_begin_frame();
    return started;
}

display_s3_profile_t display_s3_last_profile(void) {
    display_s3_profile_t profile = {0}; uint32_t sequence;
    (void)display_s3_tx_last_completed(&profile, &sequence);
    return profile;
}

bool display_s3_touch(int native_x, int native_y, int16_t *logical_x, int16_t *logical_y) {
    if (!logical_x || !logical_y || native_x < 0 || native_x >= DISPLAY_S3_WIDTH ||
        native_y < 0 || native_y >= DISPLAY_S3_HEIGHT) return false;
    *logical_x = (int16_t)(((2 * native_x + 1) * UI_W) / (2 * DISPLAY_S3_WIDTH));
    *logical_y = (int16_t)(((2 * native_y + 1) * UI_H) / (2 * DISPLAY_S3_HEIGHT));
    return true;
}

void display_s3_account_render(int64_t expand, int64_t edge, int64_t text) {
    s_profile.expand_us += expand; s_profile.edge_us += edge; s_profile.text_us += text;
}
