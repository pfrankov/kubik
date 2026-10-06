// Tess's sound voice: composed on the fly, every time. Nothing is a recording and no phrase is stored: a trigger (an app
// event or a cue from Tess's behaviour) is turned by a few rules (tess_sound_rules.c: timbre, register, contour, length)
// and five prime-length cycles into fresh notes on one D major pentatonic (D E F# A B), so overlapping sounds agree.
// The cycles have a long joint period; individual audible phrases can still coincide. The palette is deep and cosmic: fundamentals stay under
// about 1 kHz, every note is a small ensemble (an FM pair with a low index, a detuned copy that beats slowly against it,
// and a soft layer an octave down, a fifth or an octave up), slow attacks, swells and glides, a darker and longer
// reverb and a gentle low-pass over everything: pads, measured and unhurried.
// Offline contacts use celesta on two B minor pentatonic registers (F#5..E7), with pitch rising with impact speed, dry, with their own bright path.
// Character words have articulated syllables and final stress; controls retain their short tones.
// The notes are played by a small integer synth (tess_sound.c): a sine table with phase accumulators, exponential
// envelopes, up to TESS_VOICES notes at once (the voices run at half the sample rate). Portable; no samples, allocations or
// free-running oscillator. The sounds are rate limited so touching and rubbing never machine-gun, and a new sound never
// cuts the last one off: it fades in ~60 ms.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "audio.h"

#define TESS_VOICES 12
#define TESS_WORD_VOICES 6 // preserve the existing UI/character arrangement budget
#define TESS_PENDING 16  // notes composed and waiting for their time
typedef struct {
    uint32_t start;  // pending notes: the clock (samples) at which it begins
    uint32_t phase, step, mod_phase, mod_step, chorus_phase, chorus_step, layer_phase, layer_step;
    int32_t glide, mod_glide, chorus_glide, layer_glide;  // change of the four steps per voice step (half the sample rate) while glide_left counts down
    uint32_t glide_left, hold_left;
    int32_t index, index_decay;  // FM depth (phase units) and its per-voice-step factor (Q24): the swell melts into a pure tone
    int32_t env, sustain, attack_step, decay, release;  // envelope level Q30; per-sample factors Q24
    int32_t rise_left, rise_decay, partial_env, partial_decay; // celesta onset and octave, Q30/Q24
    int32_t gain;  // Q15
    int32_t chorus_share, layer_share;  // Q15 shares of the mix: the detuned copy and the layer (the carrier has the rest)
    bool bright;  // dry collision sparkle; all character/UI voices retain the warm filter
    bool harmony; // accompaniment rather than the word's syllabic lead
    uint8_t stage;  // 0 off, 1 attack, 2 decay (and hold), 3 release
} tess_voice_t;

#define TESS_REVERB_SAMPLES 4096  // 170 ms of delay lines, allocated only while Tess is selected
// What the rate limits remember. It survives tess_sound_reset: a reset must not forget how recently a cue played.
typedef struct {
    uint32_t clock;  // samples mixed so far, counted even while silent
    uint32_t cue_free_at[TC_COUNT], cue_any_free_at, event_free_at, layer_until;  // clock values
    uint8_t last_event, last_rank, last_root;
    bool last_was_cue;
} tess_gate_t;
typedef struct {
    tess_voice_t voices[TESS_VOICES], pending[TESS_PENDING];
    uint8_t active_voices, pending_count, pending_head;  // let the sample loop skip what is not there
    uint32_t random;
    uint16_t variation[5];  // prime-cycle phases: preserved by quiet/mute, seeded once at boot
    tess_gate_t gate;
    // Reverb: two damped combs and two all-pass diffusers, carved out of one buffer; a dark low-pass on its output.
    int16_t reverb[TESS_REVERB_SAMPLES];
    unsigned reverb_position[4];
    unsigned reverb_samples_left, reverb_hold;
    int32_t reverb_filter_state[3], bass_state, tone_state;  // the combs' damping, the wet low-pass; the DC block; the low-pass over the whole sound
    int32_t bright_now, bright_before; // short sparks, interpolated separately from the warm voice
    int32_t dry_now, dry_before;  // the voices' last two values (they run at half the rate; the samples between are interpolated)
    bool between, voiced;         // the next sample is the one between; the voices were sounding at the last value
    uint8_t reverb_send;  // 0..8 eighths of the dry sound sent to the reverb; 0 is a dry sound
} tess_sound_t;

void tess_sound_reset(tess_sound_t *s);  // silence: keeps the seed and the rate limits
void tess_sound_seed(tess_sound_t *s, unsigned seed);  // initialize variation once; firmware uses esp_random() at boot
// An event of the app's sound kit. `index` >= 0 picks a notch of the stepped groups (volume, detent, glint).
// `position` -1..1 is the side of the touch, `energy` 0..1 how high on the screen.
void tess_sound_event(tess_sound_t *s, sfx_t event, int index, float position, float energy);
// A cue from Tess's behaviour (tess_cue.h). False when it stayed silent: rate limited, or a stronger sound just started.
bool tess_sound_cue(tess_sound_t *s, tess_cue_t cue, float strength, float position);
bool tess_sound_cue_is_utterance(tess_cue_t cue);
bool tess_sound_event_is_utterance(sfx_t event);
bool tess_sound_active(const tess_sound_t *s);
bool tess_sound_mix(tess_sound_t *s, int32_t *out, int count);
