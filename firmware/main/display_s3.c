#include "display_s3.h"
#include "viewport.h"
#include "board.h"
#include "display_te_s3.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#define S3_HOT IRAM_ATTR
#else
#define S3_HOT
#endif

// The raw transport admits at most two pixel buffers at once. A third buffer
// can be filled before board_display_write waits for the oldest DMA completion.
#define BUFFERS 3
_Static_assert(LCD_PIPE == BUFFERS, "S3 scaler shares the raw triple-buffer ownership contract");
_Static_assert(UI_RENDER_H >= UI_H, "S3 render padding cannot crop the visible UI");
_Static_assert(DISPLAY_S3_HEIGHT % DISPLAY_S3_ROWS == 0, "Native damage bands cover the panel exactly");

typedef struct { uint16_t source, fraction; } sample_t;
static sample_t s_x_axis[DISPLAY_S3_WIDTH], s_y_axis[DISPLAY_S3_HEIGHT];
#define DAMAGE_BANDS (DISPLAY_S3_HEIGHT / DISPLAY_S3_ROWS)
static uint16_t *s_frame, *s_native, *s_dma[BUFFERS];
static uint16_t s_first_read[UI_W + 1];
static uint16_t s_left[DAMAGE_BANDS], s_right[DAMAGE_BANDS];
static uint16_t s_rows[2][DISPLAY_S3_WIDTH] __attribute__((aligned(4)));
static int s_row_key[2];
static int s_next, s_x0, s_x1, s_y0, s_y1, s_y;
static bool s_open;
static display_s3_profile_t s_profile, s_last_profile;
typedef struct { int x0, y0, x1, y1; } native_region_t;
static void prepare_logical_rows(int y0, int y1);

static void release_buffers(void) {
    heap_caps_free(s_frame); s_frame = NULL;
    heap_caps_free(s_native); s_native = NULL;
    for (int i = 0; i < BUFFERS; i++) { heap_caps_free(s_dma[i]); s_dma[i] = NULL; }
}

static void configure_axis(sample_t *axis, int logical, int native) {
    for (int i = 0; i < native; i++) {
        int position = ((2 * i + 1) * logical * 256) / (2 * native) - 128;
        axis[i] = (sample_t){(uint16_t)(position / 256), (uint16_t)(position % 256)};
    }
}

bool display_s3_init(void) {
    assert(!s_open);
    if (!s_frame) s_frame = heap_caps_calloc(UI_W * UI_H, sizeof(*s_frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) return false;
    if (!s_native) s_native = heap_caps_calloc(DISPLAY_S3_WIDTH * DISPLAY_S3_HEIGHT, sizeof(*s_native), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_native) { release_buffers(); return false; }
    for (int i = 0; i < BUFFERS; i++) {
        if (!s_dma[i]) s_dma[i] = heap_caps_malloc(DISPLAY_S3_WIDTH * DISPLAY_S3_ROWS * sizeof(*s_dma[i]),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_dma[i]) { release_buffers(); return false; }
    }
    // The rectangular UI has the panel's aspect; independent pixel-centre
    // axes only account for the integer logical-height rounding.
    configure_axis(s_x_axis, UI_W, DISPLAY_S3_WIDTH);
    configure_axis(s_y_axis, UI_H, DISPLAY_S3_HEIGHT);
    int sample = 0;
    for (int logical = 0; logical <= UI_W; logical++) {
        while (sample < DISPLAY_S3_WIDTH && s_x_axis[sample].source + 1 < logical) sample++;
        s_first_read[logical] = (uint16_t)sample;
    }
    memset(s_native, 0, DISPLAY_S3_WIDTH * DISPLAY_S3_HEIGHT * sizeof(*s_native));
    display_s3_begin_frame();
    memset(s_frame, 0, UI_W * UI_H * sizeof(*s_frame));
    s_next = 0;
    return true;
}

void display_s3_begin(int x0, int y0, int x1, int y1) {
    assert(s_frame && !s_open && x0 >= 0 && y0 >= 0 && x1 <= UI_W && y1 <= UI_RENDER_H && x0 < x1 && y0 < y1);
    s_x0 = x0; s_x1 = x1; s_y0 = s_y = y0; s_y1 = y1; s_open = true;
}

static inline uint16_t byte_swap(uint16_t value) { return (uint16_t)((value << 8) | (value >> 8)); }

void display_s3_write(const uint16_t *pixels, int count, bool last) {
    int width = s_x1 - s_x0;
    assert(s_open && pixels && count > 0 && count % width == 0);
    int rows = count / width;
    assert(rows <= s_y1 - s_y);
    int64_t started = esp_timer_get_time();
    for (int row = 0; row < rows && s_y + row < UI_H; row++) {
        uint16_t *out = s_frame + (s_y + row) * UI_W + s_x0;
        const uint16_t *in = pixels + row * width;
        memcpy(out, in, (size_t)width * sizeof(*out));
    }
    s_profile.copy_us += esp_timer_get_time() - started;
    s_profile.input_pixels += (uint32_t)count;
    // A presentation window may span the full screen. Prepare each bounded
    // incoming chunk now, before subsequent copies evict these logical rows.
    prepare_logical_rows(s_y, s_y + rows);
    s_y += rows;
    assert(last == (s_y == s_y1));
}

// First output sample whose right neighbour can read the logical start.
static int affected_start(const sample_t *axis, int length, int logical) {
    int lo = 0, hi = length;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (axis[mid].source + 1 < logical) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// First output sample whose left neighbour is beyond the logical update.
static int affected_end(const sample_t *axis, int length, int logical) {
    int lo = 0, hi = length;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (axis[mid].source < logical) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Red/blue occupy separate 16-bit lanes. The Q8 rounding bias is added
// independently in each lane, matching scalar RGB565 interpolation exactly.
static inline __attribute__((always_inline)) uint16_t mix_rgb565(uint16_t a, uint16_t b, unsigned fraction) {
    if (a == b) return a;
    int32_t rb_a = (a & 31) | ((a & 0xf800) << 5);
    int32_t rb_b = (b & 31) | ((b & 0xf800) << 5);
    uint32_t rb = (uint32_t)(rb_a + (((rb_b - rb_a) * (int32_t)fraction + 0x00800080) >> 8));
    int32_t green = (a >> 5) & 63;
    green += (((((b >> 5) & 63) - green) * (int32_t)fraction + 128) >> 8);
    return (uint16_t)(((rb >> 5) & 0xf800) | ((uint32_t)green << 5) | (rb & 31));
}

typedef uint32_t pixel_pair_t __attribute__((__may_alias__));
static inline void fill_row(uint16_t *out, int x0, int x1, uint16_t color) {
    if (x0 & 1) out[x0++] = color;
    uint32_t pair = color | ((uint32_t)color << 16);
    for (; x0 + 1 < x1; x0 += 2) *(pixel_pair_t *)(void *)(out + x0) = pair;
    if (x0 < x1) out[x0] = color;
}

static S3_HOT const uint16_t *horizontal_row(int source_y, int x0, int x1) {
    int slot = source_y & 1;
    uint16_t *out = s_rows[slot];
    if (s_row_key[slot] == source_y) return out;
    const uint16_t *source = s_frame + source_y * UI_W;
    int limit = s_x_axis[x1 - 1].source + 2;
    for (int x = x0; x < x1;) {
        const sample_t sample = s_x_axis[x];
        uint16_t color = source[sample.source];
        int end = sample.source + 1;
        while (end < limit && source[end] == color) end++;
        int same = s_first_read[end] < x1 ? s_first_read[end] : x1;
        if (same > x) { fill_row(out, x, same, color); x = same; }
        else { out[x++] = mix_rgb565(color, source[sample.source + 1], sample.fraction); }
    }
    s_row_key[slot] = source_y;
    return out;
}

static S3_HOT void scaled_row(uint16_t *out, int y, int x0, int x1) {
    const sample_t sample = s_y_axis[y];
    const uint16_t *top = horizontal_row(sample.source, x0, x1);
    const uint16_t *bottom = horizontal_row(sample.source + 1, x0, x1);
    if (!memcmp(top + x0, bottom + x0, (size_t)(x1 - x0) * 2)) {
        const pixel_pair_t *in = (const pixel_pair_t *)(const void *)(top + x0);
        pixel_pair_t *pairs = (pixel_pair_t *)(void *)out;
        for (int i = 0; i < (x1 - x0) / 2; i++) {
            uint32_t pair = in[i];
            pairs[i] = ((pair >> 8) & 0x00ff00ffu) | ((pair << 8) & 0xff00ff00u);
        }
        return;
    }
    for (int x = x0; x < x1; x++)
        *out++ = byte_swap(top[x] == bottom[x] ? top[x] : mix_rgb565(top[x], bottom[x], sample.fraction));
}

void display_s3_begin_frame(void) {
    assert(!s_open);
    memset(&s_profile, 0, sizeof s_profile);
    for (int band = 0; band < DAMAGE_BANDS; band++) { s_left[band] = DISPLAY_S3_WIDTH; s_right[band] = 0; }
}

// One dependency mapping for native preparation and accumulated send damage.
static native_region_t affected_region(int logical_y0, int logical_y1) {
    int end = logical_y1 < UI_H ? logical_y1 : UI_H;
    if (logical_y0 >= end) return (native_region_t){0};
    int x0 = affected_start(s_x_axis, DISPLAY_S3_WIDTH, s_x0);
    int x1 = affected_end(s_x_axis, DISPLAY_S3_WIDTH, s_x1);
    x0 &= ~15; x1 = (x1 + 15) & ~15;
    if (x1 > DISPLAY_S3_WIDTH) x1 = DISPLAY_S3_WIDTH;
    return (native_region_t){x0, affected_start(s_y_axis, DISPLAY_S3_HEIGHT, logical_y0),
                             x1, affected_end(s_y_axis, DISPLAY_S3_HEIGHT, end)};
}

static void prepare_logical_rows(int y0, int y1) {
    native_region_t region = affected_region(y0, y1);
    if (region.y0 >= region.y1) return;
    int64_t started = esp_timer_get_time();
    s_profile.native_pixels += (uint32_t)((region.x1 - region.x0) * (region.y1 - region.y0));
    s_row_key[0] = s_row_key[1] = -1;
    // Only the actual affected rows, not whole 8-row send bands. A following
    // chunk recalculates the shared filter boundary after its pixels change.
    for (int y = region.y0; y < region.y1; y++)
        scaled_row(s_native + y * DISPLAY_S3_WIDTH + region.x0, y, region.x0, region.x1);
    s_profile.prepare_us += esp_timer_get_time() - started;
}

void display_s3_end(void) {
    assert(s_open && s_y == s_y1);
    s_open = false;
    native_region_t region = affected_region(s_y0, s_y1);
    // Neighbouring bands share 16-pixel column bounds, with one bounded hull
    // per band. Gaps retain their correct preceding native image.
    for (int band = region.y0 / DISPLAY_S3_ROWS; band < (region.y1 + DISPLAY_S3_ROWS - 1) / DISPLAY_S3_ROWS; band++) {
        if (region.x0 < s_left[band]) s_left[band] = (uint16_t)region.x0;
        if (region.x1 > s_right[band]) s_right[band] = (uint16_t)region.x1;
    }
}

static void stream_area(int x0, int x1, int y0, int y1) {
    s_profile.windows++;
    board_display_begin(x0, y0, x1, y1);
    int width = x1 - x0;
    for (int y = y0; y < y1; y += DISPLAY_S3_ROWS) {
        uint16_t *pixels = s_dma[s_next]; s_next = (s_next + 1) % BUFFERS;
        for (int row = 0; row < DISPLAY_S3_ROWS; row++)
            memcpy(pixels + row * width, s_native + (y + row) * DISPLAY_S3_WIDTH + x0, (size_t)width * 2);
        board_display_write(pixels, width * DISPLAY_S3_ROWS, y + DISPLAY_S3_ROWS == y1);
    }
    board_display_end();
}

static void report_slow_phases(void) {
#ifdef ESP_PLATFORM
    static int64_t window_start;
    static unsigned reported;
    int64_t now = esp_timer_get_time();
    if (now - window_start >= 10000000) { window_start = now; reported = 0; }
    int64_t total = s_last_profile.copy_us + s_last_profile.prepare_us + s_last_profile.te_us + s_last_profile.burst_us;
    if (reported >= 4 || total <= 25000) return;
    reported++;
    ESP_LOGI("s3frame", "in=%u native=%u windows=%u copy=%lld prep=%lld TE=%lld burst=%lld us",
        (unsigned)s_last_profile.input_pixels, (unsigned)s_last_profile.native_pixels, (unsigned)s_last_profile.windows,
        (long long)s_last_profile.copy_us, (long long)s_last_profile.prepare_us,
        (long long)s_last_profile.te_us, (long long)s_last_profile.burst_us);
#endif
}

int64_t display_s3_present(void) {
    assert(!s_open);
    // Every logical window has already prepared its native damage, including
    // any halo changed by a later overlapping window. Only TE and DMA remain.
    bool damaged = false;
    for (int band = 0; band < DAMAGE_BANDS; band++)
        damaged |= s_left[band] < s_right[band];
    if (!damaged) { s_last_profile = s_profile; return 0; }
    int64_t phase = esp_timer_get_time();
    ESP_ERROR_CHECK(display_te_wait());
    s_profile.te_us = esp_timer_get_time() - phase;
    int64_t start = 0;
    for (int band = 0; band < DAMAGE_BANDS;) {
        int first = band++, x0 = s_left[first], x1 = s_right[first];
        if (x0 >= x1) continue;
        while (band < DAMAGE_BANDS && s_left[band] == x0 && s_right[band] == x1) band++;
        if (!start) start = esp_timer_get_time();
        stream_area(x0, x1, first * DISPLAY_S3_ROWS, band * DISPLAY_S3_ROWS);
    }
    // Attribute the DMA tail to this frame, rather than the next TE wait.
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
