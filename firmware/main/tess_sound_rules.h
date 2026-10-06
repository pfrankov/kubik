// How Tess's sounds are composed: not what they are. A rule gives one kind of sound its character (voice, register,
// contour, length, loudness) as ranges; the composer (tess_sound_compose.c) draws a fresh sound from them each time.
#pragma once
#include "tess_sound_internal.h"

// D major pentatonic, counted in scale degrees from D3: D E F# A B, then the same an octave up, and so on. No
// character/UI fundamental goes above B5 (MAX_DEGREE, 988 Hz). Offline collision
// grains have their own sparkling register, D6..D7 with small tint offsets, below 2.4 kHz.
enum {
    D3, E3, Fs3, A3, B3, D4, E4, Fs4, A4, B4, D5, E5, Fs5, A5, B5, D6, E6, Fs6, A6, B6, D7, SCALE_DEGREES
};
#define MAX_DEGREE B5
enum {  // which scale notes a sound may use (bit = degree % 5: D E F# A B); the contour climbs through these
    ANY = 0x1F,    // all five
    TRIAD = 0x0D,  // D F# A: the major arpeggio
    MINOR = 0x1D,  // D F# A B: falling to B, the relative minor's root
    FIFTHS = 0x09, // D A
    CHIME = 0x12,  // E B
    LOW_FIFTH = 0x14, // F# B
    OPEN = 0x0A,   // E A
};
enum { REPEAT, RISE, FALL, ARCH, ZIGZAG };  // how the pitch moves across the notes: level, up, down, up and back, alternating
enum {  // the voices (k_timbres): every one is a pad-like ensemble, a carrier with a detuned copy and a soft layer
    SOFT,     // a short soft swell (40 ms attack): quick reactions and the interface
    NUDGE,    // the quickest swell (28 ms): contact
    GLOW,     // a slow rich swell, a soft octave below: runs and arpeggios
    SWEEP,    // nearly pure sines that hold: glides
    PAD,      // a slow warm pad: sighs, love
    CLUSTER,  // two voices a little apart that beat slowly: unease, shaken, scared
    HUM,      // a round low hum with a third harmonic over it (a missing fundamental): lonely, offline
    TIMBRE_COUNT
};
enum {
    BY_STRENGTH = 1,  // the number of notes follows the strength, not the dice
    SCALED = 2,       // the level follows the strength
    CAPTURE = 4,      // the microphone opens with it: no reverb, and nothing else rings on
    BRIGHTEN = 8,     // the swell gets a little deeper with the strength
    HALO = 32,        // a slow pure swell an octave above the last note blooms after it
    UTTERANCE = 64,   // character speech: suppressed while recording or playing an agent reply
};
typedef struct {
    uint16_t ratio_q8, index_q8, ring_ms, attack_ms, layer_ratio_q8, layer_gain, sustain, release_ms;
    uint16_t chorus_detune, chorus_gain;  // the detuned copy: 1/1024 of the pitch (either way, by the dice) and its share (Q15)
} timbre_t;
enum { H_NONE, H_WARM, H_BRIGHT, H_SOMBER, H_TENSE, H_LOW, H_AIRY, H_COUNT };
typedef struct {
    uint8_t timbre, contour, mask, notes_min, notes_max, harmony;
    int8_t low, span;   // scale degree of the first note; ladder steps (notes of the mask) the contour covers
    int8_t pan, climb, vary, bend;  // degrees: per unit of position, per unit of strength, at most (random) from the dice; a glide within a note
    uint16_t spacing_min, spacing_max, decay_ms, gain;  // ms between notes; how long a note is audible (35 dB down; a sustained one: its length)
    uint8_t send, rank, flags;  // reverb send in eighths; 0 tick, 1 small, 2 reaction, 3 alert: a cue never talks over an equal or higher one
    uint16_t hold_ms, gap_ms;  // how long the reverb tail is kept; how soon the same sound may play again
} rule_t;
extern const timbre_t k_timbres[TIMBRE_COUNT];
extern const rule_t k_event_rules[SFX_COUNT], k_cue_rules[TC_COUNT];
