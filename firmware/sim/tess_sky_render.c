// The real face and synth, with the same bounded finger/tilt input that proves each procedural sky.
// Measures peak level and rises in sounding 100 ms windows; writes a sample for listening.
// Usage: tess_sky_render OUT.wav [ELEMENTS met at the start, 1..10]
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../main/tess_sky_play.h"
#include "../main/tess_sound.h"

enum { FPS = 30, SAMPLES = AUDIO_RATE / FPS, SECONDS = 70, WINDOW = AUDIO_RATE / 10 };
static face_t face;
static scene_t scene;
static tess_sound_t sound;
static FILE *out;
static int peak, frames, tones[TT_COUNT];
static double window_energy, windows[SECONDS * 10 + 4];
static int window_fill, window_n;

static void little(uint32_t value, int bytes) { for (int i = 0; i < bytes; i++) fputc((value >> (i * 8)) & 255, out); }
static void mix(void) {
    int32_t pcm[SAMPLES] = {0};
    tess_sound_mix(&sound, pcm, SAMPLES);
    for (int i = 0; i < SAMPLES; i++) {
        int32_t x = pcm[i] > 32767 ? 32767 : pcm[i] < -32768 ? -32768 : pcm[i];
        if (abs(x) > peak) peak = abs(x);
        window_energy += (double)x * x;
        if (++window_fill == WINDOW && window_n < (int)(sizeof windows / sizeof *windows)) {
            windows[window_n++] = 10 * log10(window_energy / WINDOW / (32768.0 * 32768.0) + 1e-12);
            window_energy = 0; window_fill = 0;
        }
        little((uint16_t)(int16_t)x, 2);
    }
}
static void frame(void) {
    face_update(&face, 1.f / FPS); face_draw(&face, &scene);
    tess_cue_t cue; float strength, position; tess_tone_t tone;
    while (face_take_cue(&face, &cue, &strength, &position)) tess_sound_cue(&sound, cue, strength, position);
    while (face_take_tone(&face, &tone)) tones[tone.kind] += tess_sound_tone(&sound, &tone);
    mix();
    frames++;
}

static void play_it(sky_world_t *w) {
    sky_force_t force;
    sky_solution(w, &force);
    face.touch_in = (touch_frame_t){0};
    float scale = w->kind == SKY_POUR ? 1.f : SKY_TILT_SCALE;
    face.grav_x = force.gx / scale; face.grav_y = force.gy / scale;
    if (force.count) face.touch_in.point[0] = (touch_point_t){.down=true,.x=force.touch[0].x,.y=force.touch[0].y};
}
static void header(void) {
    uint32_t size = SECONDS * FPS * SAMPLES * 2;
    fwrite("RIFF", 1, 4, out); little(size + 36, 4);
    fwrite("WAVEfmt ", 1, 8, out); little(16, 4); little(1, 2); little(1, 2); little(AUDIO_RATE, 4);
    little(AUDIO_RATE * 2, 4); little(2, 2); little(16, 2);
    fwrite("data", 1, 4, out); little(size, 4);
}
static void summary(void) {
    double loudest = -120, jump = 0;
    for (int i = 0; i < window_n; i++) {
        if (windows[i] > loudest) loudest = windows[i];
        if (i && windows[i - 1] > -45 && windows[i] - windows[i - 1] > jump) jump = windows[i] - windows[i - 1];
    }
    printf("peak %d/32768, loudest 100 ms %.1f dBFS, biggest rise over a sounding 100 ms window %.1f dB, skies solved %d\n", peak, loudest, jump, face.tess_games.sky_solved);
    static const char *const names[TT_COUNT] = {"ring", "creak", "twang", "thud", "grain", "whoosh", "chirp", "wobble", "pad", "invite", "hum", "sky-pluck", "sky-chime", "sky-exit"};
    for (int k = 0; k < TT_COUNT; k++) printf("  %-6s %d\n", names[k], tones[k]);
}

int main(int argc, char **argv) {
    if (argc < 2 || !(out = fopen(argv[1], "wb"))) return 1;
    header();
    face_init(&face); face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, 0x3F); face_set_mode(&face, MODE_IDLE);
    for (int i = 0; i < 3 * FPS; i++) { face_update(&face, 1.f / FPS); face_draw(&face, &scene); face_take_cue(&face, &(tess_cue_t){0}, &(float){0}, &(float){0}); }
    face.tess_games.self_wait = 99;
    face.tess_games.sky_elements = (uint8_t)(argc > 2 ? atoi(argv[2]) : 1);
    face.tess_games.sky_skill = 140;
    tess_sound_reset(&sound); tess_sound_seed(&sound, 11);
    while (!tess_sky_invite(&face)) {}
    while (frames < (int)(3.5f * FPS)) frame();
    face_event(&face, FEV_TAP, face.tess_games.sky_call[0], face.tess_games.sky_call[1]);
    while (frames < SECONDS * FPS) {
        tess_sky_t *k = face.tess_games.sky;
        if (k && k->phase == SKY_PLAY) play_it(&k->w);
        else face.touch_in = (touch_frame_t){0};
        frame();
    }
    int error = ferror(out); fclose(out);
    summary();
    return error;
}
