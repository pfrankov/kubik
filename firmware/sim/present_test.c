// The frame output against a virtual panel and clock: what the panel shows after a frame is exactly the frame that
// was composed, at any panel speed (no band of an earlier frame survives, nothing is deferred), and the panel is
// written only after the whole frame is composed, in one burst. cc via tools/test-render.py
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/present.h"
#include "../main/face.h"
#include "../main/agent_menu.h"

#ifdef PRESENT_SPLIT
#define SPLIT 1  // built with a store smaller than the dense frames
#else
#define SPLIT 0
#endif
static int64_t g_now;
static double g_us_per_px;  // panel cost of a pixel in the virtual clock
static frame_out_t *running;
static int writes_before_composed;  // panel writes that began while the frame was still being composed
int64_t esp_timer_get_time(void) { return g_now; }

static uint16_t panel[LCD_H][LCD_W];
static int win_x0, win_y0, win_x1, win_y1, win_at;
static int64_t written_px;
void display_begin(int x0, int y0, int x1, int y1) {
    if (running->bands_kept < R_BANDS) writes_before_composed++;
    win_x0 = x0; win_y0 = y0; win_x1 = x1; win_y1 = y1; win_at = 0;
}
void display_write(const uint16_t *pixels, int count, bool last) {
    (void)last;
    int width = win_x1 - win_x0;
    for (int i = 0; i < count; i++, win_at++)
        panel[win_y0 + win_at / width][win_x0 + win_at % width] = rgb565_bswap(pixels[i]);  // the panel byte order
    written_px += count;
    g_now += (int64_t)(count * g_us_per_px);
}
void display_end(void) {}

// The reference: the same scene rendered whole, expanded and overlaid, as the simulator does.
static uint16_t ref_native[R_H][R_W], ref_panel[LCD_H][LCD_W];
static void keep(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    (void)ctx;
    for (int y = y0; y < y1; y++) memcpy(&ref_native[y][x0], px + (y - y0) * (x1 - x0), (size_t)(x1 - x0) * 2);
}
static void read_ref(void *ctx, int y, uint16_t *row, int xa, int xb) { (void)ctx; memcpy(row, ref_native[y], R_W * 2); }
static void reference(const scene_t *scene) {
    static render_state_t rs;
    static uint16_t b0[R_W * R_BAND], b1[R_W * R_BAND];
    uint16_t *bufs[2] = {b0, b1};
    render_frame(&rs, scene, bufs, 2, false, keep, NULL);
    static render_output_t out;
    render_output_begin(&out, read_ref, NULL);
    render_expand_2x(&out, &ref_panel[0][0], 0, 0, LCD_W, LCD_H, false);
    render_edge_overlay(scene, &ref_panel[0][0], 0, 0, LCD_W, LCD_H, false);
    render_text_overlay(scene, &ref_panel[0][0], 0, 0, LCD_W, LCD_H, false);
}

static int wrong_rows(void) {
    int rows = 0;
    for (int y = 0; y < LCD_H; y++) rows += memcmp(panel[y], ref_panel[y], sizeof panel[y]) != 0;
    return rows;
}

static void turn_view(face_t *f, float t) {
    float angle = .9f * sinf(t * 1.1f), ax = .6f, ay = .8f * cosf(t * .7f), az = .3f * sinf(t * .4f);
    float s = sinf(angle / 2) / sqrtf(ax * ax + ay * ay + az * az);
    f->view_q[0] = cosf(angle / 2); f->view_q[1] = ax * s; f->view_q[2] = ay * s; f->view_q[3] = az * s;
}

static frame_out_t *make_output(scene_t *scene, uint16_t *bands[2]) {
    frame_out_t *o = calloc(1, sizeof *o);
    o->scene = scene;
    o->bands[0] = bands[0];
    o->bands[1] = bands[1];
    for (int i = 0; i < LCD_PIPE; i++) o->bufs[i] = malloc(LCD_W * OUTPUT_ROWS * 2);
    return o;
}

// Two seconds of a turning view, each frame shown and compared. cost: the panel's microseconds per pixel.
static void run(double cost, int64_t *burst_mean) {
    static scene_t scene;
    static render_state_t rs;
    static uint16_t b0[R_W * R_BAND] __attribute__((aligned(4))), b1[R_W * R_BAND] __attribute__((aligned(4)));
    uint16_t *bands[2] = {b0, b1};
    frame_out_t *o = make_output(&scene, bands);
    running = o;
    memset(panel, 0, sizeof panel);
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    for (int i = 0; i < 90; i++) face_update(&f, 1.f / R_FPS);
    writes_before_composed = 0;
    for (int i = 0; i < 60; i++) {
        turn_view(&f, i / (float)R_FPS);
        face_update(&f, 1.f / R_FPS);
        face_draw(&f, &scene);
        g_us_per_px = cost;
        assert(present_frame(o, &rs, &scene));
        reference(&scene);
        if (wrong_rows()) {
            printf("frame %d: %d rows of the panel differ from the composed frame\n", i, wrong_rows());
            assert(0);
        }
    }
    if (!SPLIT) assert(writes_before_composed == 0);
    assert(o->overflows == 0);
    *burst_mean = o->bursts ? o->burst_sum / o->bursts : 0;
    printf("cost %.2f us/px: 60 frames exact, panel burst avg %lld max %lld us (first to last write), packed frame max %u bytes\n", cost,
           (long long)(o->bursts ? o->burst_sum / o->bursts : 0), (long long)o->burst_max, o->store_max);
    for (int i = 0; i < LCD_PIPE; i++) free(o->bufs[i]);
    free(o);
}

// Both characters through every mode in sequence (the edge band grows and retracts, bubbles and text come and go),
// events in between: every frame the panel equals the composed frame.
static void run_modes(int character, double cost) {
    static scene_t scene;
    static render_state_t rs;
    static uint16_t b0[R_W * R_BAND] __attribute__((aligned(4))), b1[R_W * R_BAND] __attribute__((aligned(4)));
    static const face_mode_t modes[] = {MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_IDLE, MODE_SLEEP, MODE_OFFLINE, MODE_IDLE};
    uint16_t *bands[2] = {b0, b1};
    frame_out_t *o = make_output(&scene, bands);
    running = o;
    memset(panel, 0, sizeof panel);
    face_t f;
    face_init(&f);
    face_set_character(&f, (character_t)character);
    int frames = 0;
    for (size_t m = 0; m < sizeof modes / sizeof *modes; m++) {
        face_set_mode(&f, modes[m]);
        for (int i = 0; i < 45; i++, frames++) {
            if (i == 10) face_event(&f, FEV_TAP, 180, 210);
            if (i == 20) face_event(&f, FEV_NOTIFY, 0, 0);
            if (i == 30) face_event(&f, FEV_SHAKE, 0, 0);
            turn_view(&f, frames / (float)R_FPS);
            face_update(&f, 1.f / R_FPS);
            face_draw(&f, &scene);
            g_us_per_px = cost;
            assert(present_frame(o, &rs, &scene));
            reference(&scene);
            if (wrong_rows()) {
                printf("character %d mode %d frame %d: %d rows differ\n", character, (int)modes[m], i, wrong_rows());
                assert(0);
            }
        }
    }
    if (!SPLIT) assert(writes_before_composed == 0);
    assert(o->overflows == 0);
    if (SPLIT) assert(o->flushes > 0);  // the store is too small for the dense frames: they go out in two, exactly
    printf("character %d, %d frames through every mode: exact, packed frame max %u bytes\n", character, frames, o->store_max);
    for (int i = 0; i < LCD_PIPE; i++) free(o->bufs[i]);
    free(o);
}

static void draw_overlay_fixture(scene_t *scene, int i) {
    scene_begin(scene, 0);
    sc_label(scene, TXT_ACTION, "Back", 70, 50, 0, 0xF2EADC, 1);
    if (i != 8) sc_icon(scene, ICON_MIC, i == 9 ? -4 : 240, i == 9 ? -4 : 240,
                64, 0x62F0DC, i & 1 ? .5f : 1);
    sc_label(scene, TXT_ACTION, i < 5 ? "Next" : "Open settings", 330, 420, 0, 0xF2EADC, 1);
    if (i == 10) sc_label(scene, TXT_ACTION, "Back", 70, 50, 0, 0xFF8A7A, .5f);
}
static void update_menu_fixture(face_t *face, int i) {
    if (i == 10) face->menu.open = true;
    if (i == 40) agent_menu_start_guide(&face->agent, true);
    if (i >= 40 && i < 88) face->agent.guide_step = (i - 40) / 12;
    if (i == 88) { face->agent.open = false; face->menu.open = false; }
}

static void run_overlays(void) {
    static scene_t scene;
    static render_state_t rs;
    static uint16_t b0[R_W * R_BAND], b1[R_W * R_BAND];
    uint16_t *bands[2] = {b0, b1};
    frame_out_t *o = make_output(&scene, bands);
    running = o; memset(panel, 0, sizeof panel);
    g_us_per_px = 0;
    for (int i = 0; i < 12; i++) {
        draw_overlay_fixture(&scene, i);
        int64_t before = written_px;
        assert(present_frame(o, &rs, &scene));
        reference(&scene); assert(wrong_rows() == 0);
        if (i == 1) assert(written_px - before < 15000); // one icon must not repaint distant labels
    }
    face_t face; face_init(&face);
    face.menu.volume = 60; face.menu.brightness = 80;
    static const char *const words[MT_COUNT] = {"Wi-Fi", "Power off", "Reset"};
    memcpy(face.menu.txt, words, sizeof words);
    for (int i = 0; i < 100; i++) {
        update_menu_fixture(&face, i);
        face_update(&face, 1.f / R_FPS); face_draw(&face, &scene);
        assert(present_frame(o, &rs, &scene));
        reference(&scene); assert(wrong_rows() == 0);
    }
    assert(o->overflows == 0);
    for (int i = 0; i < LCD_PIPE; i++) free(o->bufs[i]);
    free(o);
    puts("overlays: isolated icon change, removal, overlap, clipped bounds, menu and guide transitions exact");
}

// Partial packet reads must not decode a later stripe-run through a truncated packet pointer.
static void run_partial_reads(void) {
    frame_store_t *store = calloc(1, sizeof *store);
    assert(store);
    uint16_t source[R_W] __attribute__((aligned(4))), out[R_W] __attribute__((aligned(4)));
    for (int x = 0; x < R_W; x++) source[x] = (uint16_t)(x * 73 + 11);
    for (int x = 0; x < 16; x++) source[x] = 0x2468;
    frame_store_begin(store);
    assert(frame_store_row(store, 0, 0x15, source)); // RLE, gap, literal, gap, another literal
    const int spans[][2] = {{2, 6}, {34, 38}, {12, 22}, {36, 70}, {68, R_W}};
    for (unsigned n = 0; n < sizeof spans / sizeof spans[0]; n++) {
        for (int x = 0; x < R_W; x++) out[x] = 0xDEAD;
        frame_store_read(store, 0, out, spans[n][0], spans[n][1]);
        for (int x = 0; x < R_W; x++) {
            bool wanted = x >= spans[n][0] && x < spans[n][1] && (0x15 & (1u << (x / FRAME_STRIPE)));
            assert(out[x] == (wanted ? source[x] : 0xDEAD));
        }
    }
    free(store);
    puts("packed reads: partial RLE/literal, stripe gaps and right boundary exact");
}

static void run_rejected(void) {
    static scene_t scene;
    static render_state_t state;
    static uint16_t band0[R_W * R_BAND], band1[R_W * R_BAND];
    uint16_t *bands[] = {band0, band1};
    frame_out_t *frame = make_output(&scene, bands);
    running = frame;
    scene_begin(&scene, 0xFFFF);
    int64_t before = written_px;
    assert(!present_frame(frame, &state, &scene));
    assert(frame->overflows == 1 && written_px == before);
    for (int i = 0; i < LCD_PIPE; i++) free(frame->bufs[i]);
    free(frame);
    puts("present: rejected frame is not acknowledged or sent");
}

int main(void) {
    if (FRAME_STORE_BYTES < 128) { run_rejected(); return 0; }
    run_partial_reads();
    int64_t burst;
    run(0.0, &burst);  // an unlimited panel
    run(0.2, &burst);  // the device's panel bus: about 0.2 us a pixel
    assert(burst < 20000);  // the mean burst of a turning view stays well inside a frame period (the first, whole-screen frame is the longest)
    run(0.6, &burst);  // a panel too slow for the frame: still exact
    run_modes(CHARACTER_TESS, 0.0);
    run_modes(CHARACTER_PLUSH, 0.0);
    run_overlays();
    puts("present ok");
    return 0;
}
