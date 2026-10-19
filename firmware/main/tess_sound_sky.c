#include <math.h>
#include "tess_sound_rules.h"

// The sounds of Skies, her game of light and harmony, in the palette of her voice. A star's pad is a held voice:
// the ensemble of its colour (teal PAD, gold GLOW, rose HUM) whose gain glides with how full the star is, so the
// music grows as the stars fill. Incoming light is a short celesta note (the bright path of the collision
// sparks), a chime is a slow swell that rings by itself, and a black hole hums softly under the others.
// Bright grains follow contacts; low accents gather real crossings into a sparse shared rhythm.
#define PAD_GAIN 1050   // a full star at full level; the sky's other stars share the room (tess_sky_sound.c)
#define HUM_GAIN 700
#define CHIME_GAIN 1500
#define PLUCK_GAIN 1000 // celesta notes are sparks: they sit on the bright path, quieter per unit of gain
#define REVERB_SEND 5

static uint32_t step_hz(float hz) { return (uint32_t)(fminf(fmaxf(hz, 100.f), 2700.f) * (4294967296.f / AUDIO_RATE)); }
static const uint8_t k_pad_timbre[3] = {PAD, GLOW, HUM};

static void bloom_reverb(tess_sound_t *s) {
    if (s->reverb_send < REVERB_SEND) s->reverb_send = REVERB_SEND;
    s->reverb_hold = AUDIO_RATE;
}

static bool pad(tess_sound_t *s, const tess_tone_t *t) {
    int colour = (int)(t->shade * 2.f + .5f);
    tess_note_t n = tess_sound_ensemble(k_pad_timbre[colour < 0 ? 0 : colour > 2 ? 2 : colour], t->hz, 1.f, PAD_GAIN);
    n.step = n.end_step = step_hz(t->hz);
    n.sustain = 32767; n.decay_ms = 40; n.attack_ms = 320; n.release_ms = 700;
    n.chorus_detune = 5; n.chorus_gain = 9000; n.body = false;
    n.sky_game = true;
    tess_sound_hold(s, t->slot, &n, (int)(PAD_GAIN * t->gain));
    if (t->gain > 0) bloom_reverb(s);
    return t->gain > 0;
}

static bool hum(tess_sound_t *s, const tess_tone_t *t) {
    tess_note_t n = tess_sound_ensemble(HUM, t->hz, 1.f, HUM_GAIN);
    n.step = n.end_step = step_hz(t->hz);
    n.sustain = 32767; n.decay_ms = 40; n.attack_ms = 500; n.release_ms = 600; n.body = false;
    n.sky_game = true;
    tess_sound_hold(s, 8u + t->slot, &n, (int)(HUM_GAIN * t->gain));
    return t->gain > 0;
}

// A star filling: a struck celesta note. It keeps the reverb the pads are in.
static bool pluck(tess_sound_t *s, const tess_tone_t *t) {
    int decay = 150 + (int)(120.f * (1.f - t->shade));
    tess_note_t n = {.bright = true, .step = step_hz(t->hz), .end_step = step_hz(t->hz), .ratio_q8 = 256, .layer_ratio_q8 = 512,
        .attack_ms = 5, .decay_ms = (uint16_t)decay, .ring_ms = (uint16_t)(decay * 55 / 100), .release_ms = (uint16_t)decay,
        .gain = (uint16_t)(PLUCK_GAIN * t->gain), .sky_game = true};
    n.delay_samples = (uint32_t)(t->delay_s * AUDIO_RATE);
    tess_sound_note(s, &n);
    return n.gain > 0;
}

// One bounded voice contains the low carrier, detuned copy and octave/twelfth, audible on the small speaker.
static bool pulse(tess_sound_t *s, const tess_tone_t *t) {
    tess_note_t n = tess_sound_ensemble(HUM, t->hz, t->gain, 1200);
    n.step = n.end_step = step_hz(t->hz);
    n.attack_ms = 24; n.decay_ms = 110; n.sustain = 8000; n.hold_ms = 40; n.release_ms = 180;
    n.ring_ms = 180; n.index_q8 = 40; n.chorus_detune = 4; n.chorus_gain = 6000;
    n.layer_ratio_q8 = t->shade > .5f ? 512 : 768; n.layer_gain = 9000;
    n.body = false; n.sky_game = true;
    tess_sound_note(s, &n);
    bloom_reverb(s);
    return n.gain > 0;
}

// A swell that rings by itself and fades: the invitation's two notes, the resolving sky's chord.
static bool chime(tess_sound_t *s, const tess_tone_t *t, bool game) {
    tess_note_t n = tess_sound_ensemble(PAD, t->hz, t->gain, CHIME_GAIN);
    n.attack_ms = 220; n.decay_ms = 400; n.hold_ms = (uint16_t)(500 + 700 * t->shade); n.sustain = 12000; n.release_ms = 900;
    if (!game) {
        n.attack_ms = 1000; n.decay_ms = 1000; n.hold_ms = 800; n.release_ms = 650;
        n.index_q8 = 0; n.layer_gain = 1800; n.chorus_gain = 4500;
    }
    n.sky_game = game;
    n.delay_samples = (uint32_t)(t->delay_s * AUDIO_RATE);
    tess_sound_note(s, &n);
    bloom_reverb(s);
    return n.gain > 0;
}

static bool valid_sky_tone(const tess_tone_t *t) {
    return t && isfinite(t->hz) && isfinite(t->gain) && t->gain >= 0 && t->slot <= 7;
}

static bool release_sky(tess_sound_t *s, const tess_tone_t *t) {
    if (t->gain > 0) return false;
    tess_sound_sky_release(s);
    return true;
}

bool tess_sound_sky(tess_sound_t *s, const tess_tone_t *t) {
    if (!valid_sky_tone(t)) return false;
    switch (t->kind) {
    case TT_SKY_PAD: return pad(s, t);
    case TT_SKY_HUM: return hum(s, t);
    case TT_SKY_INVITE: return t->gain > 0 && chime(s, t, false);
    case TT_SKY_GAME_PLUCK: return t->gain > 0 && pluck(s, t);
    case TT_SKY_GAME_PULSE: return t->gain > 0 && pulse(s, t);
    case TT_SKY_GAME_CHIME: return t->gain > 0 && chime(s, t, true);
    case TT_SKY_RELEASE: return release_sky(s, t);
    default: return false;
    }
}
