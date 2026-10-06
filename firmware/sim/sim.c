// Host simulator: renders Kubik's face with the exact firmware renderer.
//   sim sheet  > sheet.rgb      (grid of every emotion/mode, 1920x2400 rgb24)
//   sim sheet tiles > one 480x480 frame per tile
//   sim gravity | sim jitter | sim rotate   (Tess motion scenarios, see sim_motion.c)
//   sim mood [scenario]   (Tess idle moves and reactions, see sim_mood.c)
//   sim feel [mood|sheet|transitions]   (Tess's moods, see sim_feel.c)
//   sim play <scenario>   (what a finger does to Tess, and what it does alone; see sim_play.c)
//   sim gaze <scenario>   (Tess's attention: attentive, tilted, vibration, peek, wandering; see sim_mood.c)
//   sim video  > video.rgb      (scripted scenario, 480x480 rgb24 @24fps)
// Pipe through ffmpeg (see run.sh). Also prints redraw statistics to stderr.
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "../main/sprite.h"
#include "../main/qr.h"

static uint8_t *pack;
static size_t pack_len;

static const uint8_t *fetch(uint32_t off, uint32_t len, uint32_t ho, uint32_t hl) {
    (void)ho;
    (void)hl;
    return off + len <= pack_len ? pack + off : NULL;
}

void sim_load_assets(void) {
    FILE *fp = fopen(getenv("KUBIK_SPRITES") ? getenv("KUBIK_SPRITES") : "../assets/sprites.bin", "rb");
    if (!fp) {
        fprintf(stderr, "no sprite pack: drawing the classic face\n");
        return;
    }
    fseek(fp, 0, SEEK_END);
    pack_len = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    pack = malloc(pack_len);
    if (fread(pack, 1, pack_len, fp) != pack_len) pack_len = 0;
    fclose(fp);
    fprintf(stderr, "sprite pack: %zu bytes, %s\n", pack_len, sprite_init(fetch) ? "ok" : "INVALID");
}

static uint16_t fb[R_H][R_W];
uint16_t panel[PANEL_H][PANEL_W];
static uint16_t b0[R_W * R_BAND], b1[R_W * R_BAND];
static uint16_t *bufs[2] = {b0, b1};
static render_state_t rs;
static unsigned long long px_total, frames;
static unsigned px_max;

static bool s_swap;  // KUBIK_SWAP=1: render in the panel byte order (as the device does)

static void push(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    (void)ctx;
    int w = x1 - x0;
    if ((x0 & 1) || (w & 1)) fprintf(stderr, "unaligned window %d..%d\n", x0, x1);
    for (int y = y0; y < y1; y++) memcpy(&fb[y][x0], px + (y - y0) * w, w * 2);
    if (s_swap)
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) fb[y][x] = rgb565_bswap(fb[y][x]);
}

void sim_to_rgb(uint16_t c, unsigned char *o) {
    o[0] = ((c >> 11) & 31) * 255 / 31;
    o[1] = ((c >> 5) & 63) * 255 / 63;
    o[2] = (c & 31) * 255 / 31;
}

static scene_t scene;

static void read_row(void *ctx, int y, uint16_t *row, int xa, int xb) {
    (void)ctx; memcpy(row, fb[y], R_W * 2);
}
// SIM_MOOD_OFF: no moods, the neutral Tess as before them (the frame-hash streams of tools/test-frames.py that guard it).
void sim_mood_setup(face_t *f) { if (getenv("SIM_MOOD_OFF")) face_mood_off(f, true); }
void sim_render(face_t *f) {
    sim_mood_setup(f);
    face_draw(f, &scene);
    sim_render_scene(&scene);
}
void sim_render_scene(scene_t *input) {
    render_frame(&rs, input, bufs, 2, s_swap, push, NULL);
    static render_output_t output;
    render_output_begin(&output, read_row, NULL);
    render_expand_2x(&output, &panel[0][0], 0, 0, PANEL_W, PANEL_H, false);
    render_edge_overlay(input, &panel[0][0], 0, 0, PANEL_W, PANEL_H, false);
    render_text_overlay(input, &panel[0][0], 0, 0, PANEL_W, PANEL_H, false);
    px_total += rs.pixels_last;
    if (rs.pixels_last > px_max) px_max = rs.pixels_last;
    frames++;
}

void sim_render_reset(void) { rs = (render_state_t){0}; memset(fb, 0, sizeof fb); memset(panel, 0, sizeof panel); }  // a new scene: forget the frame before

void sim_emit_frame(void) {
    static unsigned char row[PANEL_W * 3];
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) sim_to_rgb(panel[y][x], row + x * 3);
        fwrite(row, 1, sizeof(row), stdout);
    }
}

// A scripted 38 s day-in-the-life scenario.
static void video_input_events(face_t *f, int i) {
    if (i == R_FPS * 3) face_event(f, FEV_TAP, 180, 210);
    if (i == R_FPS * 5) face_set_mode(f, MODE_LISTENING);
}
static void video_speech_events(face_t *f, int i) {
    if (i == R_FPS * 8) face_set_mode(f, MODE_THINKING);
    if (i == R_FPS * 10) {
        face_set_mode(f, MODE_SPEAKING);
        face_set_emotion(f, EMO_HAPPY, 5);
        face_event(f, FEV_TALK_START, 0, 0);
    }
}
static void video_later_events(face_t *f, int i) {
    if (i == R_FPS * 14) face_set_mode(f, MODE_IDLE); if (i == R_FPS * 15) face_event(f, FEV_PET, 240, 300);
    if (i == R_FPS * 18) face_event(f, FEV_VOLUME, 0.8f, 0); if (i == R_FPS * 20) face_event(f, FEV_NOT_HEARD, 0, 0);
    if (i == R_FPS * 22) face_event(f, FEV_SHAKE, 0, 0); if (i == R_FPS * 25) face_event(f, FEV_NOTIFY, 0, 0);
    if (i == R_FPS * 26) face_set_emotion(f, EMO_PROUD, 2.5f); if (i == R_FPS * 29) face_set_mode(f, MODE_SLEEP);
    if (i == R_FPS * 32) {
        face_set_mode(f, MODE_IDLE);
        face_event(f, FEV_WAKE, 0, 0);
    }
    if (i == R_FPS * 35) face_event(f, FEV_PICKUP, 1, 0);
}
static void video_microphone(face_t *f, float t) {
    if (f->mode == MODE_LISTENING) {
        float syll = fabsf(sinf(t * 7.3f)) * (0.5f + 0.5f * sinf(t * 1.9f));
        f->mic_level = 0.15f + 0.8f * syll;
    } else f->mic_level = 0;
}
static void video_speaker(face_t *f, float t) {
    if (f->mode == MODE_SPEAKING) {
        float env = fabsf(sinf(t * 11.f)) * (0.6f + 0.4f * sinf(t * 2.7f));
        f->spk_level = (fmodf(t, 1.7f) < 0.25f) ? 0 : env;
    } else f->spk_level = 0;
}
static void video(void) {
    face_t f;
    face_init(&f);
    if (getenv("SIM_CHARACTER") && !strcmp(getenv("SIM_CHARACTER"), "tess"))
        face_set_character(&f, CHARACTER_TESS);
    const float dt = 1.f / R_FPS;
    int N = R_FPS * 38;
    for (int i = 0; i < N; i++) {
        float t = i * dt;
        video_input_events(&f, i);
        video_microphone(&f, t);
        video_speech_events(&f, i);
        video_speaker(&f, t);
        video_later_events(&f, i);
        face_update(&f, dt);
        sim_render(&f);
        sim_emit_frame();
    }
    fprintf(stderr, "video: %llu frames, avg %llu px/frame (%.1f%% of screen), max %u px\n", frames, px_total / frames,
            100.0 * px_total / frames / (R_W * R_H), px_max);
}

// Per-frame face/screen alignment review: every frame of animation `name`
// with the face drawn on it, the tracked face rectangle (yellow) and the glass outline (red) on top.
static void quad_outline(const float *q, uint16_t col) {
    for (int e = 0; e < 4; e++)
        for (int i = 0; i <= 300; i++) {
            float t = i / 300.f;
            int x = (int)lrintf(q[e * 2] + (q[(e * 2 + 2) % 8] - q[e * 2]) * t);
            int y = (int)lrintf(q[e * 2 + 1] + (q[(e * 2 + 3) % 8] - q[e * 2 + 1]) * t);
            if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H) panel[y][x] = col;
        }
}
static void outline(const sprite_screen_t *sc, uint16_t col) {
    float c = cosf(sc->ang), s = sinf(sc->ang);
    for (int e = 0; e < 4; e++)
        for (int i = 0; i <= 400; i++) {
            float t = i / 200.f - 1, u = e < 2 ? t * sc->hw : (e == 2 ? -1 : 1) * sc->hw;
            float v = e < 2 ? (e == 0 ? -1 : 1) * sc->hh : t * sc->hh;
            int x = (int)lrintf(sc->cx + u * c - v * s), y = (int)lrintf(sc->cy + u * s + v * c);
            if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H) panel[y][x] = col;
        }
}
static void glass_outline(const sprite_screen_t *sc, uint16_t col) {
    for (int i = 0; i < 24 * 40; i++) {
        int k = i / 40, k2 = (k + 1) % 24;
        float t = (i % 40) / 40.f, a0 = 6.2831853f * k / 24, a1 = 6.2831853f * k2 / 24;
        float x0 = sc->gx + sc->gr[k] * cosf(a0), y0 = sc->gy + sc->gr[k] * sinf(a0);
        float x1 = sc->gx + sc->gr[k2] * cosf(a1), y1 = sc->gy + sc->gr[k2] * sinf(a1);
        int x = (int)lrintf(x0 + (x1 - x0) * t), y = (int)lrintf(y0 + (y1 - y0) * t);
        if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H) panel[y][x] = col;
    }
}
static void track(const char *name) {
    int a = sprite_find(name);
    if (a < 0) { fprintf(stderr, "no animation %s\n", name); exit(1); }
    const sprite_anim_t *an = sprite_anim(a);
    face_t f;
    face_init(&f);
    f.boot_t = 5;
    face_set_mode(&f, MODE_IDLE);
    for (int i = 0; i < 30; i++) face_update(&f, 1.f / 24);
    for (int i = 0; i < an->count; i++) {
        f.body.anim = a;
        f.body.frame = i;
        f.body_dy = 0;
        sprite_screen_t sc;
        if (sprite_screen(a, i, &sc)) {
            f.scr_cx = sc.cx; f.scr_cy = sc.cy; f.scr_hw = sc.hw; f.scr_hh = sc.hh; f.scr_ang = sc.ang; f.scr_vis = sc.vis;
            f.scr_has_quad = sc.has_quad;
            memcpy(f.scr_quad, sc.quad, sizeof sc.quad);
            f.scr_has_glass = sc.has_glass;
            f.scr_gx = sc.gx; f.scr_gy = sc.gy;
            memcpy(f.scr_gr, sc.gr, sizeof sc.gr);
        }
        sim_render(&f);
        if (sc.has_quad) quad_outline(sc.quad, 0xFFE0);
        else outline(&sc, 0xFFE0);
        if (sc.has_glass) glass_outline(&sc, 0xF800);
        sim_emit_frame();
    }
    fprintf(stderr, "%s: %d frames\n", name, an->count);
}
// Host CPU time of face_draw + render_frame; relative numbers only.
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static void bench(void) {
    face_t f;
    face_init(&f);
    if (getenv("SIM_CHARACTER") && !strcmp(getenv("SIM_CHARACTER"), "tess"))
        face_set_character(&f, CHARACTER_TESS);
    const float dt = 1.f / R_FPS;
    const char *names[] = {"idle", "listening", "thinking", "speaking"};
    const face_mode_t modes[] = {MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING};
    for (int phase = getenv("BENCH_P") ? atoi(getenv("BENCH_P")) : 0; phase < (getenv("BENCH_P") ? atoi(getenv("BENCH_P")) + 1 : 4); phase++) {
        if (phase) face_set_mode(&f, modes[phase]);
        double tot = 0, mx = 0;
        unsigned long long px = 0;
        int n = R_FPS * (getenv("BENCH_S") ? atoi(getenv("BENCH_S")) : 8);
        for (int i = 0; i < n; i++) {
            float t = i * dt;
            float lvl = 0.15f + 0.8f * fabsf(sinf(t * 7.3f)) * (0.5f + 0.5f * sinf(t * 1.9f));
            f.mic_level = phase == 1 ? lvl : 0;
            f.spk_level = phase == 3 ? lvl : 0;
            face_update(&f, dt);
            double t0 = now_us();
            face_draw(&f, &scene);
            render_frame(&rs, &scene, bufs, 2, s_swap, push, NULL);
            double us = now_us() - t0;
            tot += us; if (us > mx) mx = us;
            px += rs.pixels_last;
        }
        fprintf(stderr, "%-10s avg %6.0f us  max %6.0f us  %6llu px/frame\n", names[phase], tot / n, mx, px / n);
    }
}
// A gesture while OpenClaw is busy: the thinking pose must come back after it (body.cur per 0.25 s).
static void busy(void) {
    const face_event_t evs[] = {FEV_TAP, FEV_PET, FEV_SHAKE, FEV_PICKUP, FEV_NOTIFY};
    const char *names[] = {"tap", "pet", "shake", "pickup", "notify"};
    for (int k = 0; k < 5; k++) {
        face_t f;
        face_init(&f);
        for (int i = 0; i < 60; i++) face_update(&f, 1.f / 30);
        face_set_mode(&f, MODE_THINKING);
        for (int i = 0; i < 90; i++) face_update(&f, 1.f / 30);
        face_event(&f, evs[k], evs[k] == FEV_TAP ? 20 : 1, 20);
        fprintf(stderr, "%-7s", names[k]);
        for (int i = 0; i < 300; i++) {
            face_update(&f, 1.f / 30);
            if (i % 8 == 0) fprintf(stderr, " %d%s", f.body.cur, f.emotion == EMO_NEUTRAL ? "" : "*");
        }
        fprintf(stderr, "\n");
    }
}

static void character_preview(int character, int mode, bool menu, bool motion) {
    face_t f; face_init(&f); face_set_character(&f,character); face_set_mode(&f,mode);
    f.body.req=-1;
    f.offline_icon = getenv("KUBIK_OFFLINE") ? atoi(getenv("KUBIK_OFFLINE")) : 0;
    face_set_power(&f, true, getenv("KUBIK_BATTERY") ? atoi(getenv("KUBIK_BATTERY")) : 85, getenv("KUBIK_CHARGING") != NULL, getenv("KUBIK_CHARGING") != NULL);
    if(menu) {
        f.menu.open=true; f.menu.k=1; f.menu.volume=70; f.menu.brightness=80;
        f.menu.txt[MT_WIFI]="Wi-Fi"; f.menu.txt[MT_POWER]="Power off";
    }
    for(int i=0;i<240;i++) face_update(&f,1.f/R_FPS);
    if(getenv("KUBIK_EVENT")) {
        const char *event=getenv("KUBIK_EVENT");
        face_event(&f, !strcmp(event,"pet")?FEV_PET:!strcmp(event,"shake")?FEV_SHAKE:FEV_TAP, 300, 200);
        for(int i=0;i<30;i++) face_update(&f,1.f/R_FPS);
    }
    for(int i=0;i<(motion?R_FPS*8:1);i++) {
        f.mic_level=f.spk_level=0.5f+0.4f*sinf(i*.17f);
        face_update(&f,1.f/R_FPS); sim_render(&f); sim_emit_frame();
    }
}

// Tess states, one frame each (tile them with ffmpeg); `motion` emits a 3 s clip per state instead.
static void tess_sheet(bool motion) {
    struct { const char *name; int mode, ev, emo; float tilt_x, tilt_y, level, advance; } st[] = {
        {"idle", MODE_IDLE, -1, -1, 0, 0, 0, 3}, {"idle later", MODE_IDLE, -1, -1, 0, 0, 0, 6},
        {"tilt left", MODE_IDLE, -1, -1, -1, 0, 0, 3}, {"tilt down", MODE_IDLE, -1, -1, 0, 1, 0, 3},
        {"listening", MODE_LISTENING, -1, -1, 0, 0, .7f, 3}, {"thinking", MODE_THINKING, -1, -1, 0, 0, 0, 3},
        {"speaking", MODE_SPEAKING, -1, -1, 0, 0, .8f, 3}, {"sleep", MODE_SLEEP, -1, -1, 0, 0, 0, 3},
        {"offline", MODE_OFFLINE, -1, -1, 0, 0, 0, 3}, {"tap", MODE_IDLE, FEV_TAP, -1, 0, 0, 0, .25f},
        {"pet", MODE_IDLE, FEV_PET, -1, 0, 0, 0, 1.5f}, {"shake", MODE_IDLE, FEV_SHAKE, -1, 0, 0, 0, .6f},
        {"not heard", MODE_IDLE, FEV_NOT_HEARD, -1, 0, 0, 0, .8f}, {"fail", MODE_IDLE, FEV_FAIL, -1, 0, 0, 0, 1},
        {"joy", MODE_IDLE, -1, EMO_JOY, 0, 0, 0, .9f}, {"angry", MODE_IDLE, -1, EMO_ANGRY, 0, 0, 0, 1},
        {"shy", MODE_IDLE, -1, EMO_SHY, 0, 0, 0, 1}, {"surprised", MODE_IDLE, -1, EMO_SURPRISED, 0, 0, 0, .25f},
        {"notify", MODE_IDLE, FEV_NOTIFY, -1, 0, 0, 0, .7f}, {"assembling", MODE_IDLE, -1, -1, 0, 0, 0, .45f},
        {"love", MODE_IDLE, -1, EMO_LOVE, 0, 0, 0, 1.5f}, {"sad", MODE_IDLE, -1, EMO_SAD, 0, 0, 0, 1},
        {"sleep: drifting", MODE_SLEEP, -1, -1, 0, 0, 0, 2}, {"sleep: apart", MODE_SLEEP, -1, -1, 0, 0, 0, 5},
        {"sleep: asleep", MODE_SLEEP, -1, -1, 0, 0, 0, 12},
    };
    for (unsigned k = 0; k < sizeof st / sizeof st[0]; k++) {
        face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS); face_set_mode(&f, MODE_IDLE);
        sim_mood_setup(&f);
        float warm = !strcmp(st[k].name, "assembling") ? 0 : 3;
        for (int i = 0; i < warm * R_FPS; i++) face_update(&f, 1.f / R_FPS);
        face_set_mode(&f, st[k].mode);
        // Tilts as device turns: tilt_x about the screen's y axis, tilt_y about its x axis (0.6 rad per unit).
        float hx = st[k].tilt_y * .3f, hy = -st[k].tilt_x * .3f;
        f.view_q[0] = cosf(hypotf(hx, hy)); f.view_q[1] = sinf(hx); f.view_q[2] = sinf(hy); f.view_q[3] = 0;
        if (st[k].ev >= 0) face_event(&f, st[k].ev, 330, 190);
        if (st[k].emo >= 0) face_set_emotion(&f, st[k].emo, 2.5f);
        int n = motion ? 3 * R_FPS : (int)(st[k].advance * R_FPS);
        if (st[k].ev == -2 && !motion) n = (int)(.8f * R_FPS);
        for (int i = 0; i < n; i++) {
            f.mic_level = f.spk_level = st[k].level * (.6f + .4f * sinf(i * .5f));
            face_update(&f, 1.f / R_FPS);
            if (motion) { sim_render(&f); sim_emit_frame(); }
        }
        if (!motion) { sim_render(&f); sim_emit_frame(); }
        fprintf(stderr, "%2u %s\n", k, st[k].name);
    }
}

typedef struct { const char *name; int min_args; void (*run)(int, char **); } command_t;
static void run_character(int argc,char **argv) { character_preview(atoi(argv[2]),atoi(argv[3]),argc>4&&atoi(argv[4]),argc>5&&atoi(argv[5])); }
static void run_track(int argc,char **argv) { (void)argc; track(argv[2]); }
static void run_tess(int argc,char **argv) { (void)argv; tess_sheet(argc>2); }
static void run_sheet(int argc,char **argv) { (void)argv; sim_sheet(argc>2); }
static void run_gravity(int argc,char **argv) { (void)argc; (void)argv; sim_gravity(); }
static void run_rotate(int argc,char **argv) { (void)argc; (void)argv; sim_rotate(); }
static void run_mood(int argc,char **argv) { sim_mood(argc>2?argv[2]:NULL); }
static void run_gaze(int argc,char **argv) { sim_gaze(argc>2?argv[2]:NULL); }
static void run_feel(int argc,char **argv) { sim_feel(argc>2?argv[2]:NULL); }
static void run_play(int argc,char **argv) { sim_play(argc>2?argv[2]:NULL); }
static void run_rub(int argc,char **argv) { sim_rub(argc>2?argv[2]:NULL, getenv("SIM_CHARACTER") && !strcmp(getenv("SIM_CHARACTER"), "tess") ? CHARACTER_TESS : CHARACTER_PLUSH); }
static void run_text(int argc,char **argv) { (void)argc; (void)argv; sim_text(); }
static void run_jitter(int argc,char **argv) { (void)argc; (void)argv; sim_jitter(); }
static void run_bench(int argc,char **argv) { (void)argc; (void)argv; bench(); }
static void run_busy(int argc,char **argv) { (void)argc; (void)argv; busy(); }
static void run_dialogue(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[2], "live")) sim_live_dialogue();
    else sim_dialogue();
}
static void run_command(int argc, char **argv) {
    static const command_t commands[] = {
        {"dialogue",2,run_dialogue}, {"character",4,run_character}, {"track",3,run_track}, {"tess",2,run_tess},
        {"sheet",2,run_sheet}, {"gravity",2,run_gravity}, {"jitter",2,run_jitter}, {"mood",2,run_mood}, {"gaze",2,run_gaze}, {"rub",2,run_rub}, {"feel",2,run_feel}, {"play",3,run_play}, {"text",2,run_text}, {"rotate",2,run_rotate}, {"bench",2,run_bench}, {"busy",2,run_busy}
    };
    for (unsigned i=0;i<sizeof commands/sizeof commands[0];i++)
        if (argc>=commands[i].min_args && !strcmp(argv[1],commands[i].name)) { commands[i].run(argc,argv); return; }
    video();
}
#ifndef KUBIK_SIM_EMBEDDED
int main(int argc, char **argv) {
    s_swap = getenv("KUBIK_SWAP") && atoi(getenv("KUBIK_SWAP"));
    sim_load_assets();
    run_command(argc,argv);
    return 0;
}

#endif
