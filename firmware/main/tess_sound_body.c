#include <math.h>
#include "tess_sound_rules.h"

// The sounds of her body, in the palette of her voice. Every one is one of the established ensembles
// (k_timbres: SOFT, NUDGE, GLOW, SWEEP, PAD, CLUSTER, HUM) shaped by the physical event, never a bell or a
// tune. Pitch is continuous (tess_tone_hz), never a scale: a pluck is one GLOW resonance of her whole body, the
// corner choosing which overtone over it rings (the octave or the twelfth), sinking a little as it settles and
// beating slowly; a taut edge holds a SWEEP whose detuned copy beats faster the tauter it is; a release glides
// down to rest; the rim is a low HUM swell; a dot is a soft low NUDGE puff that sinks as it fades, no ping;
// spin is air (a beating SWEEP); her small sounds glide up or down; fear is the CLUSTER's beating. Levels sit under her words (cue classes -25..-36 dB), at most TESS_BODY_VOICES
// sound at once, each new one is ducked by those already sounding, and none takes a voice from a word.
#define BODY_SHARE_Q8 160  // the duck: each body voice already sounding scales a new one by 1 / (1 + .6 n)

static uint32_t step_hz(float hz) { return (uint32_t)(fminf(fmaxf(hz, 150.f), 1000.f) * (4294967296.f / AUDIO_RATE)); }
static uint16_t ms(float s) { return (uint16_t)fminf(fmaxf(s * 1000.f, 1.f), 4000.f); }

// One note from an ensemble of the palette, at a pitch and a level (the gain of a full-strength event).
tess_note_t tess_sound_ensemble(int timbre, float hz, float strength, int gain) {
    const timbre_t *t = &k_timbres[timbre];
    tess_note_t n = {.step = step_hz(hz), .end_step = step_hz(hz), .ratio_q8 = t->ratio_q8, .layer_ratio_q8 = t->layer_ratio_q8,
        .chorus_detune = (int16_t)t->chorus_detune, .index_q8 = t->index_q8, .ring_ms = t->ring_ms,
        .attack_ms = t->attack_ms, .release_ms = t->release_ms, .chorus_gain = t->chorus_gain, .layer_gain = t->layer_gain,
        .gain = (uint16_t)(gain * fminf(fmaxf(strength, 0), 1)), .body = true};
    return n;
}
static void glide(tess_note_t *n, float hz, float ratio, float seconds) {
    n->end_step = step_hz(hz * ratio);
    n->glide_ms = ms(seconds);
}

static tess_note_t shaped(const tess_tone_t *t) {
    float hz = t->hz, shade = fminf(fmaxf(t->shade, 0), 1), g = t->gain;
    tess_note_t n;
    switch (t->kind) {
    case TT_RING:   // her body resonating: one swell that sinks a little as it settles; the corner picks the overtone
        n = tess_sound_ensemble(GLOW, hz, g, 1550); glide(&n, hz, .985f, .8f);
        n.layer_ratio_q8 = shade < .5f ? 512 : 768; n.layer_gain = 6000;
        n.chorus_detune = (int16_t)(4 + 10 * shade);
        n.decay_ms = ms(.3f + .25f * shade); n.release_ms = 320; break;
    case TT_CREAK:  // a taut edge: held, beating faster the tauter it is
        n = tess_sound_ensemble(SWEEP, hz, g, 2500); glide(&n, hz, 1.06f, .35f);
        n.chorus_detune = (int16_t)(7 + 30 * shade); n.sustain = 22000; n.decay_ms = 8; n.hold_ms = 260; break;
    case TT_TWANG:  // let go: down to rest
        n = tess_sound_ensemble(SWEEP, hz * (1.f + .35f * shade), g, 1870); glide(&n, hz, 1.f, .25f + .25f * shade);
        n.sustain = 20000; n.decay_ms = 8; n.hold_ms = ms(.2f + .2f * shade); n.release_ms = 260; break;
    case TT_THUD:   // the rim: a low swell (the speaker hears its third harmonic), firmer the harder
        n = tess_sound_ensemble(HUM, hz, g, 3600); n.attack_ms = 28; n.index_q8 = (uint16_t)(60 + 90 * shade);
        n.decay_ms = ms(.12f + .1f * shade); n.release_ms = 220; break;
    case TT_GRAIN:  // one dot: a soft puff that sinks as it fades, never a ping
        n = tess_sound_ensemble(NUDGE, hz, g, 2150); glide(&n, hz, .8f, .09f);
        n.layer_ratio_q8 = 512; n.decay_ms = 35; n.release_ms = 90; break;
    case TT_WHOOSH: // air past her edges
        n = tess_sound_ensemble(SWEEP, hz, g, 1630); glide(&n, hz, 1.12f, .3f);
        n.chorus_detune = 26; n.chorus_gain = 13000; n.attack_ms = 70; n.decay_ms = 70; n.release_ms = 220; break;
    case TT_CHIRP:  // her small sounds: a step up when she asks, down when she sighs or grumbles
        n = tess_sound_ensemble(SOFT, hz, g, 1340); glide(&n, hz, shade > .5f ? 1.122f : .891f, .18f);
        n.decay_ms = 90; n.release_ms = 200; break;
    default:        // TT_WOBBLE: afraid, the cluster's slow beating
        n = tess_sound_ensemble(CLUSTER, hz, g, 970); glide(&n, hz, .97f, .3f); n.decay_ms = 120; n.release_ms = 220; break;
    }
    n.delay_samples = (uint32_t)(fminf(fmaxf(t->delay_s, 0), 1) * AUDIO_RATE);
    return n;
}

static int body_sounding(const tess_sound_t *s) {
    int n = 0;
    for (int i = 0; i < TESS_VOICES; i++) n += s->voices[i].stage && s->voices[i].body;
    for (int i = s->pending_head; i < s->pending_count; i++) n += s->pending[i].body;
    return n;
}

bool tess_sound_tone(tess_sound_t *s, const tess_tone_t *tone) {
    if (!tone) return false;
    if (tone->kind == TT_MOTION) return tess_sound_motion(s, tone);
    if (tone->kind >= TT_SKY_PAD && tone->kind < TT_COUNT) return tess_sound_sky(s, tone);
    if (tone->kind >= TT_COUNT || !isfinite(tone->hz) || !isfinite(tone->gain) || tone->gain <= 0) return false;
    tess_note_t note = shaped(tone);
    int sounding = body_sounding(s);
    if (sounding >= TESS_BODY_VOICES) return false;  // full: no queueing up behind, no sudden pile
    note.gain = (uint16_t)(note.gain * 256 / (256 + BODY_SHARE_Q8 * sounding));
    if (!note.gain) return false;
    tess_sound_note(s, &note);
    if (s->reverb_send < 3) {  // the body rings into the same room as her voice
        s->reverb_send = 3;
        s->reverb_hold = (unsigned)(AUDIO_RATE / 2);
    }
    return true;
}
