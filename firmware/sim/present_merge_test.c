// A wider DMA rectangle may read unstored pixels only when they are known black.
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../main/present.c"
int64_t esp_timer_get_time(void) { return 1; }
static frame_out_t frame;
static uint16_t source[R_W], out[R_W];
static bool capture;
static int window_x, window_y, window_width, panel_offset, captured;

static uint16_t average(uint16_t a, uint16_t b) {
    unsigned r = ((a >> 11) + (b >> 11)) / 2;
    unsigned g = (((a >> 5) & 63) + ((b >> 5) & 63)) / 2;
    unsigned blue = ((a & 31) + (b & 31)) / 2;
    return (uint16_t)(r << 11 | g << 5 | blue);
}
static uint16_t native_at(int x, int y) { return y <= 16 ? source[x] : 0; }
static uint16_t horizontal(int x, int y) {
    uint16_t a = native_at(x / 2, y);
    return x & 1 ? average(a, native_at(x / 2 + 1, y)) : a;
}
void display_begin(int a, int b, int c, int d) {
    window_x = a; window_y = b; window_width = c - a; panel_offset = 0;
}
void display_write(const uint16_t *p, int n, bool last) {
    if (!capture) return;
    for (int i = 0; i < n; i++, panel_offset++) {
        int x = window_x + panel_offset % window_width, y = window_y + panel_offset / window_width;
        uint16_t expected = horizontal(x, y / 2);
        if (y & 1) expected = average(expected, horizontal(x, y / 2 + 1));
        assert(rgb565_bswap(p[i]) == expected);
        ++captured;
    }
}
void display_end(void) {}

static void prepare(void) {
    memset(&frame, 0, sizeof frame);
    frame_store_begin(&frame.store);
    memset(source, 0, sizeof source);
    for (int x = 48; x < 96; x++) source[x] = (uint16_t)(x * 13);
    for (int y = 0; y <= 16; y++) {
        assert(frame_store_row(&frame.store, y, 0x38, source));
        frame.canvas.row_lit[y] = 0x38;
    }
    frame.plan_n[0] = frame.plan_n[1] = 1;
    frame.plan[0][0] = (render_win_t){64, 0, 192, 16};
    frame.plan[1][0] = (render_win_t){68, 16, 188, 32};
}
static void known_black(void) {
    prepare();
    assert(merge_frame_windows(&frame));
    render_win_t box = frame.plan[0][0];
    assert(box.x0 == 64 && box.x1 == 192 && box.y0 == 0 && box.y1 == 32);
    assert(frame.plan_skip[0] == 0 && frame.plan_skip[1] == 1);
    for (int x = 0; x < R_W; x++) out[x] = 0xDEAD;
    read_merged(&frame.store, 8, out, 32, 98);
    for (int x = 32; x < 98; x++) assert(out[x] == source[x]);
    assert(out[31] == 0xDEAD && out[98] == 0xDEAD);
}
static void unknown_pixels(void) {
    prepare();
    frame.canvas.row_lit[8] |= 1u << 2; // missing stripe could contain a stationary dot
    assert(!merge_frame_windows(&frame));
    assert(!frame.plan_skip[0] && !frame.plan_skip[1]);
    prepare();
    frame.store.stripes[8] &= ~(1u << 4); // an unknown gap inside the lit hull
    assert(!merge_frame_windows(&frame));
    prepare();
    frame.canvas.row_lit[16] |= 1u << 6; // interpolation neighbour is also required
    assert(!merge_frame_windows(&frame));
}
static void sparse_or_empty(void) {
    prepare();
    frame.plan[0][0] = (render_win_t){0, 0, 32, 16};
    frame.plan[1][0] = (render_win_t){448, 16, 480, 32};
    assert(!merge_frame_windows(&frame));
    frame.plan[1][0] = (render_win_t){64, 464, 192, 480};
    assert(!merge_frame_windows(&frame)); // a vertical gap is not a neighbour
    memset(frame.plan_n, 0, sizeof frame.plan_n);
    assert(!merge_frame_windows(&frame));
}
static void multi_hop(void) {
    prepare();
    for (int y = 17; y <= 24; y++) {
        assert(frame_store_row(&frame.store, y, 0x38, source));
        frame.canvas.row_lit[y] = 0x38;
    }
    frame.plan_n[2] = 1;
    frame.plan[2][0] = (render_win_t){72, 32, 184, 48};
    assert(merge_frame_windows(&frame));
    assert(frame.plan[0][0].y1 == 48);
    assert(!frame.plan_skip[0] && frame.plan_skip[1] == 1 && frame.plan_skip[2] == 1);
}
static void maximum_roots(void) {
    memset(&frame, 0, sizeof frame);
    for (int y = 0; y < R_H; y++) frame.canvas.row_lit[y] = UINT16_MAX; // all unstored pixels remain unknown
    for (int band = 0; band < R_BANDS; band++) {
        frame.plan_n[band] = PLAN_WINDOWS;
        for (int i = 0; i < PLAN_WINDOWS; i++)
            frame.plan[band][i] = (render_win_t){i * 40, band * 16 + 2, i * 40 + 20, band * 16 + 16};
    }
    assert(!merge_frame_windows(&frame));
    for (int band = 0; band < R_BANDS; band++) assert(!frame.plan_skip[band]);
}
static void overlapping_rows(void) {
    prepare();
    frame.plan_n[0] = 2; frame.plan_n[1] = 0;
    frame.plan[0][0] = (render_win_t){64, 0, 120, 16};
    frame.plan[0][1] = (render_win_t){120, 0, 192, 16};
    assert(merge_frame_windows(&frame));
    assert(frame.plan_skip[0] == 2);
    assert(frame.plan[0][0].x0 == 64 && frame.plan[0][0].x1 == 192);
    prepare();
    frame.plan_n[0] = 2; frame.plan_n[1] = 0;
    frame.plan[0][0] = (render_win_t){64, 0, 128, 16};
    frame.plan[0][1] = (render_win_t){96, 4, 192, 16};
    assert(merge_frame_windows(&frame)); // duplicate panel pixels need only one transfer
    assert(frame.plan_skip[0] == 2 && frame.plan[0][0].y1 == 16);
    prepare();
    frame.plan_n[0] = 2; frame.plan_n[1] = 0;
    frame.plan[0][1] = (render_win_t){32, 0, 92, 16};
    frame.canvas.row_lit[0] |= 1u << 1;
    assert(!merge_frame_windows(&frame)); // a cheaper overlapping box still cannot read unknown pixels
}

static void reset_metadata(void) {
    prepare();
    frame.store.row[100] = UINT32_MAX;
    frame_store_begin(&frame.store);
    for (int y = 0; y < R_H; y++) assert(!frame.store.stripes[y]);
    out[0] = 0xDEAD;
    frame_store_read(&frame.store, 100, out, 0, 2);
    assert(out[0] == 0xDEAD); // no stale row offset is evaluated
}
static void expanded_root(void) {
    prepare();
    frame.plan_n[0] = 3; frame.plan_n[1] = 0;
    frame.plan[0][0] = (render_win_t){0, 0, 100, 16};
    frame.plan[0][1] = (render_win_t){180, 0, 200, 16};
    frame.plan[0][2] = (render_win_t){80, 0, 200, 16};
    render_win_t box;
    assert(join_saving(&frame.plan[0][0], &frame.plan[0][1], &box) <= 0);
    assert(merge_frame_windows(&frame));
    assert(frame.plan_skip[0] == 6); // the widened root also owns the middle root
    assert(frame.plan[0][0].x0 == 0 && frame.plan[0][0].x1 == 200);
}
static void merged_output(void) {
    static scene_t scene;
    static uint16_t dma[LCD_PIPE][LCD_W * OUTPUT_ROWS];
    prepare();
    frame.scene = &scene;
    for (int i = 0; i < LCD_PIPE; i++) frame.bufs[i] = dma[i];
    for (int band = 0; band < R_BANDS; band++) frame.plan_reach[band] = -1;
    // A normal, separate window follows the admitted merged window. Its
    // stored black stripes must use the same reader and output-cache route.
    for (int y = 16; y <= 24; y++) {
        uint16_t pixels[R_W] = {0};
        if (y == 16) memcpy(pixels, source, sizeof pixels);
        assert(frame_store_row(&frame.store, y, (y == 16 ? 0x38 : 0) | 0x600, pixels));
    }
    frame.plan_n[2] = 1;
    frame.plan[2][0] = (render_win_t){300, 32, 316, 48};
    captured = 0; capture = true;
    output_frame(&frame);
    capture = false;
    assert(frame.output.read == read_merged && frame.windows == 2);
    assert(captured == 128 * 32 + 16 * 16);
}
int main(void) {
    known_black(); unknown_pixels(); sparse_or_empty(); multi_hop(); maximum_roots(); overlapping_rows(); expanded_root(); reset_metadata(); merged_output();
    puts("DMA merge: exact black gaps, unknown rejection, multi-hop/max roots, overlap, reset and mixed-window DMA output");
}
