// An 18-second native-renderer conversation: idle, waiting, syllables/pauses, return.
#include <math.h>
#include <stdio.h>
#include "sim.h"

void sim_dialogue(void) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
    for (int i = 0; i < 90; i++) face_update(&f, 1.f / R_FPS);
    for (int i = 0; i < 18 * R_FPS; i++) {
        float t = i / (float)R_FPS;
        if (i == 3 * R_FPS) face_set_mode(&f, MODE_THINKING);
        if (i == 6 * R_FPS) face_set_mode(&f, MODE_SPEAKING);
        if (i == 13 * R_FPS) face_set_mode(&f, MODE_IDLE);
        float sentence = fmodf(t - 6, 1.9f);
        f.spk_level = f.mode == MODE_SPEAKING && sentence > .28f
            ? .08f + .82f * fabsf(sinf(t * 9.4f)) * (.7f + .3f * sinf(t * 2.1f)) : 0;
        face_update(&f, 1.f / R_FPS);
        sim_render(&f); sim_emit_frame();
    }
    fprintf(stderr, "dialogue: 540 frames, playback envelope is simulated\n");
}

// Persistent Live: connecting, mic, thinking, playback, mic again, explicit end.
void sim_live_dialogue(void) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    for (int i = 0; i < 90; i++) face_update(&f, 1.f / R_FPS);
    f.live_active = true; face_set_mode(&f, MODE_THINKING);
    for (int i = 0; i < 12 * R_FPS; i++) {
        float t = i / (float)R_FPS;
        if (i == R_FPS) { f.live_ready = f.live_mic = true; face_set_mode(&f, MODE_LISTENING); }
        if (i == 4 * R_FPS) { face_set_mode(&f, MODE_THINKING); }
        if (i == 6 * R_FPS) face_set_mode(&f, MODE_SPEAKING);
        if (i == 9 * R_FPS) { f.live_mic = true; face_set_mode(&f, MODE_LISTENING); }
        if (i == 11 * R_FPS) { f.live_active = f.live_ready = f.live_mic = false; face_set_mode(&f, MODE_IDLE); }
        f.mic_level = f.live_mic && fmodf(t, 1.7f) > .4f ? .7f * fabsf(sinf(t * 8)) : 0;
        f.spk_level = f.mode == MODE_SPEAKING && fmodf(t, 1.5f) > .3f ? .8f * fabsf(sinf(t * 9)) : 0;
        face_update(&f, 1.f / R_FPS); sim_render(&f); sim_emit_frame();
    }
    fprintf(stderr, "live dialogue: 360 frames, synthetic microphone/playback envelopes\n");
}
