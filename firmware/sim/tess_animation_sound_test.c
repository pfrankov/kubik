// Motion stays visual; actual plucks, pulls and releases retain finite body notes.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"
#include "../main/tess_sound.h"

static face_t face;
static scene_t scene;
static tess_sound_t sound;
static unsigned tones, audible;
static int32_t pcm[AUDIO_RATE / 60];

static void frame(void) {
    face_update(&face, 1.f / 60);
    face_draw(&face, &scene);
    tess_tone_t tone;
    while (face_take_tone(&face, &tone)) {
        assert(tone.kind < TT_SKY_PAD); // held music belongs only to an active Skies round
        assert(tone.gain > 0);
        tones++;
        tess_sound_tone(&sound, &tone);
    }
    memset(pcm, 0, sizeof pcm);
    tess_sound_mix(&sound, pcm, AUDIO_RATE / 60);
    bool nonzero = false;
    for (unsigned i = 0; i < sizeof pcm / sizeof *pcm; i++) nonzero |= pcm[i] != 0;
    audible += nonzero;
    for (int i = 0; i < TESS_VOICES; i++) assert(!sound.voices[i].held);
}

static void fresh(void) {
    face_init(&face);
    face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, 0x3f);
    face_set_mode(&face, MODE_IDLE);
    tess_sound_reset(&sound);
    tess_sound_seed(&sound, 7);
    for (int i = 0; i < 180; i++) frame();
    face.tess_games.self_wait = face.tess_next_fidget = 99;
    tones = audible = 0;
    tess_sound_reset(&sound);
}

static void moving_geometry_does_not_generate_sound(void) {
    fresh();
    face.tess_mood.drag_speed[0] = 12;
    float angle = face.tess_mood.drag_angle[0];
    for (int i = 0; i < 480; i++) {
        float a = i / 60.f;
        face.grav_x = .2f * sinf(a * 2);
        face.grav_y = .15f * cosf(a * 2);
        face.view_q[0] = cosf(a * .3f);
        face.view_q[1] = sinf(a * .3f);
        frame();
    }
    assert(fabsf(face.tess_mood.drag_angle[0] - angle) > .3f);
    assert(!tones && !audible && !tess_sound_active(&sound));
}

static void body_reactions_sound_and_finish(void) {
    const tess_preview_t cases[] = {TESS_PLAY_PLUCK, TESS_PLAY_STRETCH, TESS_PLAY_SELF};
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        fresh();
        assert(tess_games_start(&face, cases[c], 0));
        for (int i = 0; i < 180; i++) frame();
        assert(tones > 0 && audible > 0);
        unsigned before = audible;
        for (int i = 0; i < 180; i++) frame();
        assert(audible == before && !tess_sound_active(&sound));
    }
}

int main(void) {
    moving_geometry_does_not_generate_sound();
    body_reactions_sound_and_finish();
    puts("animation sound: motion is silent; body reactions are finite and still audible");
}
