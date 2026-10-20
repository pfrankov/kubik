#include "tess_sound_rules.h"
#include <math.h>

#define MOTION_SLOT 16 // separate from the seven stars and four wells
#define MOTION_GAIN 1450

static bool held_motion(const tess_sound_t *s) {
    for (int i = 0; i < TESS_VOICES; i++) {
        const tess_voice_t *v = &s->voices[i];
        if (v->held && v->slot == MOTION_SLOT && v->stage && v->stage != 3) return true;
    }
    return false;
}

static bool motion_room(const tess_sound_t *s) {
    int body = 0, spare = 0;
    for (int i = 0; i < TESS_VOICES; i++) {
        body += s->voices[i].stage && s->voices[i].body;
        spare += i >= TESS_WORD_VOICES && !s->voices[i].stage;
    }
    for (int i = s->pending_head; i < s->pending_count; i++) body += s->pending[i].body;
    return body < TESS_BODY_VOICES && spare > 0;
}

bool tess_sound_motion(tess_sound_t *s, const tess_tone_t *t) {
    if (!isfinite(t->hz) || !isfinite(t->gain) || !isfinite(t->shade) || t->gain < 0) return false;
    bool active = held_motion(s);
    if (!t->gain) {
        tess_sound_hold(s, MOTION_SLOT, NULL, 0);
        return active;
    }
    if (!active) {
        if (!motion_room(s)) return false;
        s->motion_variant = (uint16_t)tess_sound_random(s);
    }
    unsigned colour = s->motion_variant;
    float shade = fminf(fmaxf(t->shade, 0), 1);
    float register_scale = .94f + .12f * (colour & 255u) / 255.f;
    tess_note_t n = tess_sound_ensemble(PAD, t->hz * register_scale, 1, MOTION_GAIN);
    // One layered resonance, not a succession of notes: material stays stable,
    // while geometry opens/closes its detuned partials and shifts its register.
    static const uint16_t layers[] = {384, 512, 768};
    n.layer_ratio_q8 = layers[(colour >> 8) % 3];
    n.layer_gain = (uint16_t)(6000 + 1800 * ((colour >> 10) & 3u));
    n.chorus_gain = 7000;
    n.chorus_detune = (int16_t)(3 + ((colour >> 12) & 3u) + 12 * shade);
    n.index_q8 = (uint16_t)(24 + ((colour >> 4) & 31u));
    n.ring_ms = 0; // held material keeps its overtones, rather than decaying to a sine
    n.attack_ms = 70; n.decay_ms = 40; n.release_ms = 65;
    int gain = (int)(MOTION_GAIN * fminf(t->gain, 1));
    tess_sound_hold(s, MOTION_SLOT, &n, gain);
    return held_motion(s);
}
