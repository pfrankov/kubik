#include "display_s3.h"
#include "viewport.h"
#include "board.h"
#include "frame_pacing_s3.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#define BUFFERS 3
#define DAMAGE_BANDS (DISPLAY_S3_HEIGHT / DISPLAY_S3_ROWS)
_Static_assert(LCD_PIPE == BUFFERS, "Native staging shares raw DMA buffer ownership");
_Static_assert(UI_RENDER_W >= DISPLAY_S3_WIDTH && UI_RENDER_H == DISPLAY_S3_HEIGHT,
               "Only the renderer's right-hand stripe padding is clipped");
static uint16_t *s_native, *s_dma[BUFFERS];
static uint16_t s_left[DAMAGE_BANDS], s_right[DAMAGE_BANDS];
static int s_next, s_x0, s_x1, s_y0, s_y1, s_y;
static bool s_open;
static int64_t s_render_start;
static display_s3_profile_t s_profile, s_last_profile;

static void release_buffers(void) {
    heap_caps_free(s_native); s_native = NULL;
    for (int i = 0; i < BUFFERS; i++) { heap_caps_free(s_dma[i]); s_dma[i] = NULL; }
}

bool display_s3_init(void) {
    assert(!s_open);
    if (!s_native) s_native = heap_caps_calloc(UI_RENDER_W * DISPLAY_S3_HEIGHT,
                                               sizeof(*s_native), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_native) return false;
    for (int i = 0; i < BUFFERS; i++) {
        if (!s_dma[i]) s_dma[i] = heap_caps_malloc(DISPLAY_S3_WIDTH * DISPLAY_S3_ROWS * sizeof(*s_dma[i]),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_dma[i]) { release_buffers(); return false; }
    }
    memset(s_native, 0, UI_RENDER_W * DISPLAY_S3_HEIGHT * sizeof(*s_native));
    s_next = 0;
    display_s3_begin_frame();
    return true;
}

void display_s3_begin(int x0, int y0, int x1, int y1) {
    assert(s_native && !s_open && x0 >= 0 && y0 >= 0 && x1 <= UI_RENDER_W &&
           y1 <= DISPLAY_S3_HEIGHT && x0 < x1 && y0 < y1);
    s_x0 = x0; s_x1 = x1; s_y0 = s_y = y0; s_y1 = y1; s_open = true;
    s_render_start = esp_timer_get_time();
}

uint16_t *display_s3_next_row(void) {
    assert(s_open && s_y < s_y1);
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
    int x0 = s_x0, x1 = s_x1 < DISPLAY_S3_WIDTH ? s_x1 : DISPLAY_S3_WIDTH;
    if (x0 >= x1) return;
    x0 &= ~15; x1 = (x1 + 15) & ~15;
    if (x1 > DISPLAY_S3_WIDTH) x1 = DISPLAY_S3_WIDTH;
    for (int band = s_y0 / DISPLAY_S3_ROWS; band < (s_y1 + DISPLAY_S3_ROWS - 1) / DISPLAY_S3_ROWS; band++) {
        if (x0 < s_left[band]) s_left[band] = (uint16_t)x0;
        if (x1 > s_right[band]) s_right[band] = (uint16_t)x1;
    }
}

static void stream_area(int x0, int x1, int y0, int y1) {
    s_profile.windows++;
    board_display_begin(x0, y0, x1, y1);
    int width = x1 - x0;
    for (int y = y0; y < y1; y += DISPLAY_S3_ROWS) {
        uint16_t *pixels = s_dma[s_next]; s_next = (s_next + 1) % BUFFERS;
        for (int row = 0; row < DISPLAY_S3_ROWS; row++)
            memcpy(pixels + row * width, s_native + (y + row) * UI_RENDER_W + x0, (size_t)width * 2);
        board_display_write(pixels, width * DISPLAY_S3_ROWS, y + DISPLAY_S3_ROWS == y1);
    }
    board_display_end();
}

static bool has_damage(void) {
    for (int band = 0; band < DAMAGE_BANDS; band++) if (s_left[band] < s_right[band]) return true;
    return false;
}

static void report_slow_phases(void) {
#ifdef ESP_PLATFORM
    static int64_t window_start;
    static unsigned reported;
    int64_t now = esp_timer_get_time();
    if (now - window_start >= 10000000) { window_start = now; reported = 0; }
    int64_t total = s_last_profile.render_us + s_last_profile.te_us + s_last_profile.burst_us;
    if (reported >= 4 || total <= 25000) return;
    reported++;
    ESP_LOGI("s3frame", "in=%u native=%u windows=%u render=%lld TE=%lld burst=%lld us",
        (unsigned)s_last_profile.input_pixels, (unsigned)s_last_profile.native_pixels, (unsigned)s_last_profile.windows,
        (long long)s_last_profile.render_us, (long long)s_last_profile.te_us, (long long)s_last_profile.burst_us);
#endif
}

int64_t display_s3_present(void) {
    assert(!s_open);
    if (!has_damage()) { s_last_profile = s_profile; return 0; }
    int64_t phase = esp_timer_get_time();
    ESP_ERROR_CHECK(frame_pacing_s3_sync());
    s_profile.te_us = esp_timer_get_time() - phase;
    int64_t start = 0;
    for (int band = 0; band < DAMAGE_BANDS;) {
        int first = band++, x0 = s_left[first], x1 = s_right[first];
        if (x0 >= x1) continue;
        while (band < DAMAGE_BANDS && s_left[band] == x0 && s_right[band] == x1) band++;
        if (!start) start = esp_timer_get_time();
        stream_area(x0, x1, first * DISPLAY_S3_ROWS, band * DISPLAY_S3_ROWS);
    }
    board_display_wait_idle();
    s_profile.burst_us = esp_timer_get_time() - start;
    s_last_profile = s_profile;
    report_slow_phases();
    display_s3_begin_frame();
    return start;
}

display_s3_profile_t display_s3_last_profile(void) { return s_last_profile; }

bool display_s3_touch(int native_x, int native_y, int16_t *logical_x, int16_t *logical_y) {
    if (!logical_x || !logical_y || native_x < 0 || native_x >= DISPLAY_S3_WIDTH ||
        native_y < 0 || native_y >= DISPLAY_S3_HEIGHT) return false;
    *logical_x = (int16_t)(((2 * native_x + 1) * UI_W) / (2 * DISPLAY_S3_WIDTH));
    *logical_y = (int16_t)(((2 * native_y + 1) * UI_H) / (2 * DISPLAY_S3_HEIGHT));
    return true;
}
