// A scripted play session through the real face and the real synth, mixed as the device mixes it: the app's
// tap sounds, Tess's cues and the sounds of her body in one voice. Plucks (one, then three quick ones), swipes
// until she blooms, a stretch and twang, the hardest throw (rim, shatter), a sweep gathering her, a tilt that
// rolls her into the rim, knocks and a burst of rapid taps. Prints the peak, the loudest 100 ms and the biggest
// jump between neighbouring 100 ms windows, so a listen can be checked by numbers too.
// Usage: tess_play_render OUT.wav
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "../main/tess_games_internal.h"
#include "../main/tess_sound.h"

enum { FPS = 30, SAMPLES = AUDIO_RATE / FPS, SECONDS = 34, WINDOW = AUDIO_RATE / 10 };
static face_t face;
static scene_t scene;
static tess_sound_t sound;
static FILE *out;
static int peak, frames, tones[TT_COUNT], cues;
static double window_energy, windows[SECONDS * 10 + 4];
static int window_fill, window_n;

static void little(uint32_t value, int bytes) { for (int i = 0; i < bytes; i++) fputc((value >> (i * 8)) & 255, out); }
static void frame(void) {
    face_update(&face, 1.f / FPS); face_draw(&face, &scene);
    tess_cue_t cue; float strength, position; tess_tone_t tone;
    while (face_take_cue(&face, &cue, &strength, &position)) cues += tess_sound_cue(&sound, cue, strength, position);
    while (face_take_tone(&face, &tone)) tones[tone.kind] += tess_sound_tone(&sound, &tone);
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
    frames++;
}
static void run(float seconds) { for (int i = 0; i < (int)(seconds * FPS); i++) frame(); }
static void until(float seconds) { while (frames < (int)(seconds * FPS)) frame(); }
static void finger(bool down, float x, float y) { face.rub_in = (rub_input_t){.down = down, .x = x, .y = y}; frame(); }
static void tap(float x, float y) {  // as app_control.c: the app's tap sound, then the face's reaction
    finger(true, x, y); finger(true, x, y); finger(false, x, y);
    tess_sound_event(&sound, SFX_TAP, -1, (x - 240) / 240, 1 - y / 480);
    face_event(&face, FEV_TAP, x, y);
}

static void script(void) {
    until(1); tap(300, 255);
    until(3); tap(175, 200); run(.1f); tap(305, 310); run(.1f); tap(240, 300);
    until(5);
    for (int s = 0; s < 6; s++) { for (int i = 0; i < 4; i++) finger(true, 170 + i * 45, 255); finger(false, 350, 255); }
    until(9); finger(true, 375, 255);
    for (int i = 0; i < 25; i++) finger(true, 375 + i * 2, 255);
    finger(false, 425, 255);
    until(12); finger(true, 400, 255);
    for (int x = 400; x <= 470; x += 10) finger(true, x, 255);
    for (int i = 0; i < 8; i++) finger(true, 470, 255);
    finger(false, 470, 255);
    run(2); finger(true, 20, 20);
    for (float y = 20; y <= 470 && face.tess_games.phase == TESS_GAME_WAIT; y += 60)
        for (float x = 20; x <= 460; x += 40) finger(true, (int)y / 60 % 2 ? 480 - x : x, y);
    finger(false, 240, 255);
    until(21); face.grav_x = .7f; run(1.5f); face.grav_x = -.7f; run(1.5f); face.grav_x = 0;
    until(26); tess_temper_jolt(&face, .12f); run(.6f); tess_temper_jolt(&face, .05f);
    until(29); for (int i = 0; i < 6; i++) { tap(240, 255); run(.25f); }
    until(SECONDS);
}

int main(int argc, char **argv) {
    if (argc != 2 || !(out = fopen(argv[1], "wb"))) return 1;
    uint32_t size = SECONDS * FPS * SAMPLES * 2;
    fwrite("RIFF", 1, 4, out); little(size + 36, 4);
    fwrite("WAVEfmt ", 1, 8, out); little(16, 4); little(1, 2); little(1, 2); little(AUDIO_RATE, 4);
    little(AUDIO_RATE * 2, 4); little(2, 2); little(16, 2);
    fwrite("data", 1, 4, out); little(size, 4);
    face_init(&face); face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, 0x3F); face_set_mode(&face, MODE_IDLE);
    for (int i = 0; i < 3 * FPS; i++) { face_update(&face, 1.f / FPS); face_draw(&face, &scene); face_take_cue(&face, &(tess_cue_t){0}, &(float){0}, &(float){0}); }
    face.tess_games.self_wait = 99;
    tess_sound_reset(&sound); tess_sound_seed(&sound, 11);
    script();
    int error = ferror(out); fclose(out);
    double loudest = -120, jump = 0;
    for (int i = 0; i < window_n; i++) {
        if (windows[i] > loudest) loudest = windows[i];
        if (i && windows[i - 1] > -45 && windows[i] - windows[i - 1] > jump) jump = windows[i] - windows[i - 1];
    }
    printf("peak %d/32768, loudest 100 ms %.1f dBFS, biggest rise over a sounding 100 ms window %.1f dB, %d cues\n", peak, loudest, jump, cues);
    static const char *const names[TT_COUNT] = {"ring", "creak", "twang", "thud", "grain", "whoosh", "chirp", "wobble", "pad", "pluck", "chime", "hum"};
    for (int k = 0; k < TT_COUNT; k++) printf("  %-6s %d\n", names[k], tones[k]);
    return error;
}
