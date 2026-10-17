// A Skies session through the real face and the real synth, mixed as the device mixes it: the summoning star and its
// motif, a tap on it, then skies played as a player would (a finger for the first, a tilt for a pour, tools glided to
// their places, relays turned a step at a time, an aimed source turned), each resolving into its own melody. Prints the
// peak, the loudest 100 ms and the biggest jump between neighbouring 100 ms windows, so a listen can be checked by
// numbers too.
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

// What a player would do in this sky.
static void turn_relays(sky_world_t *w) {
    for (int s = 0; s < w->count_stars; s++) {
        sky_star_t *st = &w->star[s];
        if (st->relay && st->dir != st->sdir && w->t > 2.5f + s && fmodf(w->t, .7f) < 1.f / FPS) st->dir = (uint8_t)((st->dir + 1) % SKY_DIRS);
    }
}
static bool alone(const sky_world_t *w) {
    if (w->count_tools || w->source.aim) return false;
    for (int s = 0; s < w->count_stars; s++) if (w->star[s].relay) return false;
    return true;
}
static void play_it(sky_world_t *w) {
    const tess_games_t *g = &face.tess_games;
    face.rub_in = (rub_input_t){0};
    face.grav_x = g->tilt_rest[0]; face.grav_y = g->tilt_rest[1];
    if (w->kind == SKY_POUR) { face.grav_x += w->gravity[0] * 1.15f; face.grav_y += w->gravity[1] * 1.15f; return; }
    turn_relays(w);
    if (alone(w)) { face.rub_in = (rub_input_t){.down = true, .x = w->star[0].x, .y = w->star[0].y}; return; }
    for (int s = 0; s < w->count_tools && w->t > 3.f; s++) {
        sky_tool_t *t = &w->tool[s];
        if (!t->movable) continue;
        t->x += (t->sx - t->x) * .05f; t->y += (t->sy - t->y) * .05f;
        t->dx += (t->sdx - t->dx) * .05f; t->dy += (t->sdy - t->dy) * .05f;
    }
    if (w->source.aim && w->t > 2.f) { w->source.dx = w->source.sdx; w->source.dy = w->source.sdy; }
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
    static const char *const names[TT_COUNT] = {"ring", "creak", "twang", "thud", "grain", "whoosh", "chirp", "wobble", "pad", "pluck", "chime", "hum"};
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
        else face.rub_in = (rub_input_t){0};
        frame();
    }
    int error = ferror(out); fclose(out);
    summary();
    return error;
}
