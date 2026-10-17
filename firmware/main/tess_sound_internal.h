// What the synth (tess_sound.c) offers the composer (tess_sound_compose.c). Not for the rest of the firmware.
#pragma once
#include "tess_sound.h"

// One composed note, in units the composer thinks in. Steps are phase increments per sample (2^32 = one cycle).
typedef struct {
    bool bright;                          // short collision sparks bypass the warm low-pass
    bool harmony;                         // accompaniment; the syllabic lead remains independently identifiable
    bool body;                            // a sound of her body: only a free voice, never one a word is using
    uint32_t delay_samples, step, end_step;  // from the start of the sound; pitch at the start and at the end of the glide
    uint16_t ratio_q8, layer_ratio_q8;        // the modulator's and the layer's frequency, in carriers (Q8)
    int16_t chorus_detune;                    // the detuned copy's pitch: this many 1/1024 of the note's above it (below: negative)
    uint16_t index_q8;                        // FM depth at the start, radians (Q8)
    uint16_t attack_ms, decay_ms, ring_ms, hold_ms, release_ms;  // ring: how fast the FM depth melts away
    uint16_t glide_ms;                        // how long the pitch takes from step to end_step
    uint16_t gain, chorus_gain, layer_gain, sustain;  // Q15: level; the shares of the detuned copy and the layer; level the decay settles at
} tess_note_t;

unsigned tess_sound_random(tess_sound_t *s);  // 0..65535, from the seed
void tess_sound_note(tess_sound_t *s, const tess_note_t *note);  // queue a note (it plays at its delay, whatever was queued before)
void tess_sound_release(tess_sound_t *s);  // everything still sounding lets go within ~60 ms, what has not begun never will
void tess_sound_fade_reverb(tess_sound_t *s);  // the tail follows in 200 ms instead of its full length
// Held voices (a game's pads): one per slot, gliding to `goal` (Q15). A goal of 0 lets it go; it also lets go by itself
// when not tended for about 0.8 s, so a lost sender never leaves a pad ringing.
void tess_sound_hold(tess_sound_t *s, unsigned slot, const tess_note_t *note, int goal);
tess_note_t tess_sound_ensemble(int timbre, float hz, float strength, int gain);  // a note of the palette at a pitch and a level
