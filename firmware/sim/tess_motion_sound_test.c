// Real projection -> held synthesis: duration, changing geometry, silence and ownership.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/tess_internal.h"
#include "../main/tess_sound.h"

static face_t face;
static scene_t scene;
static tess_sound_t sound;
static tess_projected_t dots[TESS_N];
static int32_t pcm[AUDIO_RATE / 60];
static unsigned updates;
static float gain, pitch_low, pitch_high;

static int render_audio(void) {
    tess_tone_t tone;
    while (face_take_tone(&face, &tone)) {
        if (tone.kind != TT_MOTION) continue;
        assert(tone.gain >= 0 && tone.gain <= .8f);
        gain = tone.gain;
        if (tone.gain) {
            updates++;
            pitch_low = fminf(pitch_low, tone.hz); pitch_high = fmaxf(pitch_high, tone.hz);
        }
        tess_sound_tone(&sound, &tone);
    }
    memset(pcm, 0, sizeof pcm);
    tess_sound_mix(&sound, pcm, AUDIO_RATE / 60);
    int peak = 0;
    for (unsigned i = 0; i < sizeof pcm / sizeof *pcm; i++) {
        int v = abs(pcm[i]); if (v > peak) peak = v;
    }
    assert(peak < 12000 && sound.active_voices <= TESS_BODY_VOICES); // includes existing impact transients
    return peak;
}

static void fresh(void) {
    face_init(&face); face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, 0x3f);
    face_set_mode(&face, MODE_IDLE);
    tess_sound_reset(&sound); tess_sound_seed(&sound, 7);
    updates = 0; gain = 0; pitch_low = 10000; pitch_high = 0;
}
static int frame(void) {
    face_update(&face, 1.f / 60); face_draw(&face, &scene);
    return render_audio();
}
static void settle(void) {
    for (int i = 0; i < 180; i++) frame();
    face.tess_games.self_wait = 99; face.tess_next_fidget = 99;
    face.tess_play.cue_n = 0; face.tess_play.tone_n = 0;
    tess_sound_reset(&sound);
    updates = 0;
}
static void constant_movement(float px_per_second, int frames) {
    for (int i = 0; i < frames; i++) {
        face.t += .05f;
        for (int p = 0; p < TESS_N; p++) dots[p].x += (int)(px_per_second * .4f);
        tess_motion_sound_observe(&face, dots);
        for (int n = 0; n < 3; n++) render_audio();
    }
}
static void a_motion_is_a_continuous_voice_not_repeated_notes(void) {
    fresh();
    for (int i = 0; i < TESS_N; i++) dots[i] = (tess_projected_t){1600 + i * 4, 2040, 20, 0, 0};
    tess_motion_sound_observe(&face, dots);
    face.tess_rigid = 1;
    constant_movement(10, 20);
    assert(!tess_sound_active(&sound)); // the endless rigid idle turn leaves room to listen
    face.tess_touch_t = 0;
    constant_movement(10, 20);
    float quiet = gain;
    constant_movement(80, 20);
    assert(gain > quiet * 3 && sound.active_voices == 1);
    assert(updates >= 20 && updates <= 40 && pitch_high - pitch_low > 10);
    for (int i = 0; i < 100; i++) { // five seconds: well beyond a one-shot or the held watchdog
        constant_movement(50, 1);
        assert(sound.active_voices == 1 && render_audio() > 5);
    }
    face.tess_touch_t = 10;
    face.tess_mood.drag_speed[0] = .1f; // the released momentum still owns the full tail
    constant_movement(10, 20);
    assert(sound.active_voices == 1);
    face.tess_mood.drag_speed[0] = 0;
    constant_movement(10, 30); // back to ordinary idle turning: no indefinite sustain
    assert(!face.tess_motion_sound.sounding && !tess_sound_active(&sound));
    face.tess_touch_t = 0;
    constant_movement(10, 20);
    assert(sound.active_voices == 1);
    constant_movement(0, 30);
    assert(!face.tess_motion_sound.sounding && !tess_sound_active(&sound));
    // A long invisible interval is a fresh baseline, never a sudden high-energy jump.
    face.t += 2; dots[0].x += 1000;
    tess_motion_sound_observe(&face, dots); render_audio();
    assert(!tess_sound_active(&sound));
}
static void real_animations_keep_their_resonance(void) {
    const tess_preview_t cases[] = {TESS_PLAY_BLOOM, TESS_PLAY_STRETCH, TESS_PLAY_SELF};
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        fresh(); settle();
        assert(tess_games_start(&face, cases[c], 0));
        int moving = 0, audible = 0;
        for (int i = 0; i < 120; i++) {
            int peak = frame();
            assert(peak < 2000); // the isolated sustained resonance is quieter
            if (i > 15 && face.tess_motion_sound.speed > 8) {
                moving++; audible += peak > 1;
            }
        }
        fprintf(stderr, "motion: animation %d, %d/%d moving frames audible, %u controls\n",
                cases[c], audible, moving, updates);
        assert(moving > 20 && audible == moving && updates > 10);
    }
}
static void device_rotation_is_motion_too(void) {
    fresh(); settle(); face.tess_rigid = 1;
    face.tess_touch_t = 10; face.tess_games.shaping = false;
    memset(&face.tess_mood, 0, sizeof face.tess_mood);
    memset(face.tess_reaction, 0, sizeof face.tess_reaction);
    tess_motion_sound_stop(&face);
    for (int i = 0; i < 60; i++) {
        float a = i * .025f;
        face.view_q[0] = cosf(a); face.view_q[1] = sinf(a);
        face.view_q[2] = face.view_q[3] = 0;
        face.t += .05f;
        tess_project_dots(&face, dots);
        tess_motion_sound_observe(&face, dots);
        for (int n = 0; n < 3; n++) render_audio();
    }
    assert(updates > 40 && tess_sound_active(&sound));
    for (int i = 0; i < 4; i++) face.view_q[i] = -face.view_q[i];
    constant_movement(10, 30); // same quaternion orientation, idle turning cannot retain the sound
    assert(!face.tess_motion_sound.sounding && !tess_sound_active(&sound));
}
static void quiet_boundary(int boundary) {
    switch (boundary) {
        case 0: face.dark = true; break;
        case 1: face.dimmed = true; break;
        case 2: face.menu.open = true; break;
        case 3: face.menu.k = 1; break;
        case 4: face.mode = MODE_SETUP; break;
        case 5: face.mode = MODE_LISTENING; break;
        case 6: face.mode = MODE_SPEAKING; break;
        case 7: face.mode = MODE_SLEEP; break;
        case 8: face.live_active = true; break;
        case 9: face.tess_fallen = true; break;
        case 10: face.card_n = 1; break;
        default: face.mode = MODE_THINKING; break;
    }
}
static void every_quiet_boundary_releases_before_drawing(void) {
    for (int boundary = 0; boundary < 12; boundary++) {
        fresh(); face.tess_motion_sound.sounding = true;
        tess_tone_t t = {.kind = TT_MOTION, .hz = 300, .gain = .4f};
        assert(tess_sound_tone(&sound, &t));
        quiet_boundary(boundary);
        face_update(&face, 1.f / 60); // deliberately no draw: hidden screens cannot leave a held sound
        render_audio();
        assert(!face.tess_motion_sound.sounding);
        for (int i = 0; i < TESS_VOICES; i++) assert(!sound.voices[i].held);
        for (int i = 0; i < 60; i++) render_audio();
        assert(!tess_sound_active(&sound));
    }
    fresh(); face.tess_games.game = TESS_GAME_SKY;
    tess_motion_sound_observe(&face, dots);
    assert(!face.tess_motion_sound.ready); // the sky owns its music
}
static void release_survives_a_full_queue_and_coalesces(void) {
    fresh();
    tess_tone(&face, TT_MOTION, 200, .1f, .2f);
    tess_tone(&face, TT_MOTION, 400, .5f, .8f);
    assert(face.tess_play.tone_n == 1 && face.tess_play.tone[0].shade == .8f);
    face.tess_play.tone_n = 0;
    for (int i = 0; i < TESS_TONES; i++) tess_tone(&face, TT_RING, 300, .5f, 0);
    face.tess_motion_sound.sounding = true; face.dark = true;
    tess_motion_sound_gate(&face);
    tess_tone_t tone;
    assert(face_take_tone(&face, &tone) && tone.kind == TT_MOTION && tone.gain == 0);
    assert(!face_take_tone(&face, &tone));
}
static tess_voice_t *motion_voice(void) {
    for (int i = TESS_WORD_VOICES; i < TESS_VOICES; i++)
        if (sound.voices[i].held && sound.voices[i].body) return &sound.voices[i];
    return NULL;
}
static void retuning_keeps_phase_and_new_gestures_vary(void) {
    fresh();
    tess_tone_t tone = {.kind = TT_MOTION, .hz = 260, .gain = .5f, .shade = .2f};
    assert(tess_sound_tone(&sound, &tone));
    for (int i = 0; i < 15; i++) render_audio();
    tess_voice_t *v = motion_voice(); assert(v && v->gain > 0);
    uint32_t phase = v->phase, old_pitch = v->step, random = sound.random;
    int32_t envelope = v->env;
    tone.hz = 430; tone.shade = .9f; tone.gain = .2f;
    assert(tess_sound_tone(&sound, &tone));
    assert(v->phase == phase && v->env == envelope && v->step == old_pitch);
    assert(v->glide > 0 && sound.random == random && sound.active_voices == 1);
    for (int i = 0; i < 15; i++) render_audio();
    assert(v->step > old_pitch * 1.5f && v->gain < 400);
    // Losing the sender must never leave a stuck continuous sound.
    for (int i = 0; i < 120; i++) render_audio();
    assert(!tess_sound_active(&sound));
}
static void variants_and_capacity(void) {
    fresh();
    tess_tone_t tone = {.kind = TT_MOTION, .hz = 260, .gain = .5f, .shade = .2f};
    unsigned variants[12];
    for (int i = 0; i < 12; i++) {
        tess_sound_reset(&sound);
        assert(tess_sound_tone(&sound, &tone)); variants[i] = sound.motion_variant;
        for (int j = 0; j < i; j++) assert(variants[i] != variants[j]);
    }
    tess_sound_reset(&sound);
    // Existing impacts finish; the motion sender retries instead of cutting one off.
    tess_tone_t ring = {.kind = TT_RING, .hz = 300, .gain = .4f};
    for (int i = 0; i < 3; i++) assert(tess_sound_tone(&sound, &ring));
    unsigned random = sound.random;
    assert(!tess_sound_tone(&sound, &tone) && sound.random == random && !motion_voice());
    render_audio(); // previously accepted notes keep their place before held motion retries
    assert(sound.active_voices == TESS_BODY_VOICES);
    assert(!tess_sound_tone(&sound, &tone) && !motion_voice());
    for (int i = 0; i < 240; i++) render_audio();
    assert(tess_sound_tone(&sound, &tone) && motion_voice());
}
static void idle_motion_leaves_real_silence(void) {
    fresh();
    int quiet = 0, streak = 0, longest = 0;
    for (int i = 0; i < 60 * 60; i++) {
        frame();
        if (!tess_sound_active(&sound)) { quiet++; streak++; }
        else streak = 0;
        if (streak > longest) longest = streak;
    }
    fprintf(stderr, "motion: idle silence %.2fs / 60s, longest %.2fs\n", quiet / 60.f, longest / 60.f);
    assert(quiet >= 900 && longest >= 180); // real multi-second listening windows, not a perpetual quiet bed
}
int main(void) {
    a_motion_is_a_continuous_voice_not_repeated_notes();
    real_animations_keep_their_resonance();
    device_rotation_is_motion_too();
    every_quiet_boundary_releases_before_drawing();
    release_survives_a_full_queue_and_coalesces();
    retuning_keeps_phase_and_new_gestures_vary();
    variants_and_capacity();
    idle_motion_leaves_real_silence();
    puts("motion sound: sustained geometry, inertia, dynamics, quiet boundaries and bounded queue passed");
}
