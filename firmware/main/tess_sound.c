#include "tess_sound_internal.h"
#include <math.h>
#include <string.h>

// One cycle of a sine (a table of 256 and one to interpolate with, 514 bytes), built once.
#define SINE_SIZE 256
static int16_t s_sine[SINE_SIZE + 1];
static void initialize_sine_table(void) {
    static bool done;
    if (done) return;
    for (int i = 0; i < SINE_SIZE; i++) s_sine[i] = (int16_t)lrintf(32767.f * sinf((float)(2 * M_PI) * i / SINE_SIZE));
    s_sine[SINE_SIZE] = s_sine[0];
    done = true;
}
static inline int32_t sine(uint32_t phase) {
    const int16_t *q = &s_sine[phase >> 24];
    return q[0] + (((q[1] - q[0]) * (int32_t)((phase >> 16) & 255)) >> 8);
}

enum { OFF, ATTACK, DECAY, RELEASE };  // envelope stages
#define ENV_ONE (1 << 30)
#define ENV_FLOOR (1 << 22)   // -48 dB: a voice that decays below this is over
#define UNIT_Q24 (1 << 24)
#define QUICK_RELEASE_MS 12   // time constant of the fade-out when a new sound takes over
#define PHASE_PER_RADIAN_Q8 2670177  // one radian in phase units (2^32 / 2 pi), per 1/256 of it
#define INDEX_FLOOR (1 << 20)        // an FM depth below this (.0015 rad) is gone

// The voices run at half the sample rate (a voice makes one value for two output samples; the ones between are
// interpolated, and the gentle low-pass over the sound hides the images): what a note needs is under 3 kHz, and the
// synth costs half. Everything a voice counts in (envelope, hold, glide, wobble) is in these half-rate steps; phases
// and pitch steps stay per output sample and advance twice per voice step.
#define VOICE_DIV 2
static int32_t samples_from_ms(int ms) { return ms * AUDIO_RATE / 1000; }
static int32_t voice_steps_from_ms(int ms) { return samples_from_ms(ms) / VOICE_DIV > 1 ? samples_from_ms(ms) / VOICE_DIV : 1; }
// The factor per voice step of an exponential with time constant `ms` (Q24; first order, exact enough over 120 steps or more).
static int32_t decay_factor(int ms) { return UNIT_Q24 - UNIT_Q24 / voice_steps_from_ms(ms); }
static inline int32_t times_q24(int32_t x, int32_t factor) { return (int32_t)(((int64_t)x * factor) >> 24); }
static uint32_t times_q8(uint32_t step, uint32_t ratio_q8) { return (uint32_t)(((uint64_t)step * ratio_q8) >> 8); }

void tess_sound_reset(tess_sound_t *s) {
    tess_gate_t gate = s->gate;
    uint32_t random = s->random;
    uint16_t variation[5];
    memcpy(variation, s->variation, sizeof variation);
    memset(s, 0, sizeof *s);
    s->gate = gate;
    s->random = random ? random : 1;
    memcpy(s->variation, variation, sizeof variation);
    initialize_sine_table();
}
// Mutually prime periods: their joint state repeats after 1,236,756,393,559 draws.
// Only the composer advances these counters, never the PCM loop.
static const uint16_t k_variation_period[5] = {251, 257, 263, 269, 271};
static uint32_t variation_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
void tess_sound_seed(tess_sound_t *s, unsigned seed) {
    s->random = seed ? seed : 1;
    for (int i = 0; i < 5; i++)
        s->variation[i] = variation_hash(s->random + (unsigned)i) % k_variation_period[i];
}
unsigned tess_sound_random(tess_sound_t *s) {
    uint32_t mix = 2166136261u;
    for (int i = 0; i < 5; i++) {
        if (++s->variation[i] == k_variation_period[i]) s->variation[i] = 0;
        mix = (mix ^ s->variation[i]) * 16777619u;
    }
    s->random = variation_hash(mix);
    return (s->random >> 8) & 0xFFFF;
}

// Everything still sounding lets go within ~60 ms: no click, no queue.
void tess_sound_release(tess_sound_t *s) {
    int32_t quick = decay_factor(QUICK_RELEASE_MS);
    s->pending_count = s->pending_head = 0;
    for (int i = 0; i < TESS_VOICES; i++) {
        tess_voice_t *v = &s->voices[i];
        if (!v->stage) continue;
        v->stage = RELEASE;
        if (v->release > quick) v->release = quick;
    }
}
// The reverb tail ends in a 200 ms linear fade (see mix_reverb_sample); joining it at once is continuous.
void tess_sound_fade_reverb(tess_sound_t *s) {
    if (s->reverb_samples_left > 4800) s->reverb_samples_left = 4800;
    s->reverb_hold = 4800;
}

void tess_sound_note(tess_sound_t *s, const tess_note_t *n) {
    initialize_sine_table();  // (a sound can never start before the table exists, whatever was called first)
    if (s->pending_count >= TESS_PENDING) return;
    int32_t glide_samples = n->glide_ms ? voice_steps_from_ms(n->glide_ms) : 0, pitch_change = (int32_t)(n->end_step - n->step);
    int32_t glide = glide_samples ? pitch_change / glide_samples : 0;
    uint32_t chorus = n->step + (n->step >> 10) * (uint32_t)(int32_t)n->chorus_detune;
    tess_voice_t v = {
        .start = s->gate.clock + n->delay_samples,
        .step = n->step, .mod_step = times_q8(n->step, n->ratio_q8), .layer_step = times_q8(n->step, n->layer_ratio_q8),
        .chorus_step = chorus,
        .glide = glide, .mod_glide = (int32_t)(((int64_t)glide * n->ratio_q8) >> 8),
        .layer_glide = (int32_t)(((int64_t)glide * n->layer_ratio_q8) >> 8),
        .chorus_glide = glide + (glide >> 10) * n->chorus_detune, .glide_left = (uint32_t)(glide ? glide_samples : 0),
        .hold_left = n->hold_ms ? (uint32_t)voice_steps_from_ms(n->hold_ms) : UINT32_MAX,
        .index = (int32_t)n->index_q8 * PHASE_PER_RADIAN_Q8, .index_decay = n->ring_ms ? decay_factor(n->ring_ms) : UNIT_Q24,
        .sustain = (int32_t)n->sustain << 15, .attack_step = ENV_ONE / voice_steps_from_ms(n->attack_ms),
        .decay = decay_factor(n->decay_ms), .release = decay_factor(n->release_ms ? n->release_ms : n->decay_ms),
        .gain = n->gain, .chorus_share = n->chorus_gain, .layer_share = n->layer_gain,
        .stage = ATTACK, .harmony = n->harmony, .bright = n->bright};
    if (n->bright) {
        v.stage = DECAY; v.env = v.partial_env = v.rise_left = ENV_ONE;
        v.rise_decay = decay_factor(n->attack_ms);
        v.partial_decay = decay_factor(n->ring_ms);
    }
    int at = s->pending_count++;  // kept in the order they begin
    for (; at > s->pending_head && (int32_t)(s->pending[at - 1].start - v.start) > 0; at--) s->pending[at] = s->pending[at - 1];
    s->pending[at] = v;
}

bool tess_sound_active(const tess_sound_t *s) {
    bool ringing = s->bass_state >= 32 || s->bass_state <= -32;  // Q8 filter residual: allow it to settle to its final silent step
    return s->active_voices > 0 || s->pending_head < s->pending_count || s->reverb_samples_left > 0 || ringing;
}

// Word preemption prefers released notes, preserving a freshly started lead.
static int quietest_word_voice(const tess_sound_t *s) {
    int slot = -1;
    for (int i = 0; i < TESS_WORD_VOICES; i++)
        if (s->voices[i].stage == RELEASE && (slot < 0 || s->voices[i].env < s->voices[slot].env)) slot = i;
    if (slot >= 0) return slot;
    slot = 0;
    for (int i = 1; i < TESS_WORD_VOICES; i++)
        if (s->voices[i].env < s->voices[slot].env) slot = i;
    return slot;
}

// Collisions use spare voices and finish naturally; saturation drops the new grain.
static void start_note(tess_sound_t *s, const tess_voice_t *v) {
    int slots = v->bright ? TESS_VOICES : TESS_WORD_VOICES;
    int slot = -1;
    for (int i = 0; i < slots && slot < 0; i++) if (!s->voices[i].stage) slot = i;
    if (slot < 0 && v->bright) return;
    if (slot >= 0) s->active_voices++;
    else slot = quietest_word_voice(s);
    s->voices[slot] = *v;
}

// The envelope one sample on: attack to full, exponential decay towards the sustain, hold, release; false once it has
// died away (the voice is then free).
static inline bool step_envelope(tess_sound_t *s, tess_voice_t *v) {
    switch (v->stage) {
    case ATTACK:
        v->env += v->attack_step;
        if (v->env >= ENV_ONE) { v->env = ENV_ONE; v->stage = DECAY; }
        break;
    case DECAY:
        v->env = v->sustain + times_q24(v->env - v->sustain, v->decay);
        if (!v->hold_left--) v->stage = RELEASE;
        break;
    default:
        v->env = times_q24(v->env, v->release);
    }
    if (v->stage >= DECAY && v->env < ENV_FLOOR) {
        v->stage = OFF;
        s->active_voices--;
        return false;
    }
    return true;
}

// Celesta: a gently struck body and a faint octave which rings for less time.
// The smooth squared onset matches the selected audition without a sharp click.
static inline int32_t celesta_sample(tess_sound_t *s, tess_voice_t *v) {
    int32_t decay = v->stage == RELEASE ? v->release : v->decay;
    v->env = times_q24(v->env, decay);
    v->partial_env = times_q24(v->partial_env, v->stage == RELEASE ? v->release : v->partial_decay);
    v->rise_left = times_q24(v->rise_left, v->rise_decay);
    if (v->env < ENV_FLOOR) { v->stage = OFF; s->active_voices--; return 0; }
    int32_t onset = (ENV_ONE - v->rise_left) >> 15;
    onset = onset * onset >> 15;
    int32_t body = sine(v->phase) * (v->env >> 15) >> 15;
    int32_t octave = sine(v->layer_phase) * (v->partial_env >> 15) >> 15;
    int32_t x = (body * 27769 + octave * 4999) >> 15; // octave amplitude 0.18
    v->phase += v->step * VOICE_DIV;
    v->layer_phase += v->layer_step * VOICE_DIV;
    return ((x * onset >> 15) * v->gain) >> 15;
}

// One voice, one sample: an FM pair (a modulator at a whole-number ratio bends the carrier's phase; its depth melts
// away, so the swell becomes a pure tone), a detuned copy of the carrier that beats slowly against it and a layer (an
// octave down, a fifth or an octave up). Nothing in it is noise and nothing wobbles.
static inline int32_t voice_sample(tess_sound_t *s, tess_voice_t *v) {
    if (v->bright) return celesta_sample(s, v);
    if (!step_envelope(s, v)) return 0;
    int32_t level = v->env >> 15;
    if (v->glide_left) {
        v->step += (uint32_t)v->glide; v->mod_step += (uint32_t)v->mod_glide; v->layer_step += (uint32_t)v->layer_glide;
        v->chorus_step += (uint32_t)v->chorus_glide;
        v->glide_left--;
    }
    uint32_t modulation = 0;
    if (v->index) {
        modulation = (uint32_t)(int32_t)(((int64_t)sine(v->mod_phase) * v->index) >> 15);
        v->index = v->index < INDEX_FLOOR ? 0 : times_q24(v->index, v->index_decay);  // (a melted swell stops costing)
        v->mod_phase += v->mod_step * VOICE_DIV;
    }
    int32_t x = sine(v->phase + modulation) * (32768 - v->chorus_share - v->layer_share);
    if (v->chorus_share) { x += sine(v->chorus_phase + modulation) * v->chorus_share; v->chorus_phase += v->chorus_step * VOICE_DIV; }
    if (v->layer_share) { x += sine(v->layer_phase) * v->layer_share; v->layer_phase += v->layer_step * VOICE_DIV; }
    v->phase += v->step * VOICE_DIV;
    return (((x >> 15) * level >> 15) * v->gain) >> 15;
}

#define REVERB_DAMPING 3      // /16: the one-pole low-pass in each comb's loop (about 800 Hz)
#define REVERB_FEEDBACK 57    // /64: a long tail
#define REVERB_WET_LOWPASS 5  // /16: and another over the reverb's output (about 1.4 kHz)
#define TONE_LOWPASS 85       // /256: the gentle low-pass over everything (about 1.6 kHz): no sound has a bright edge
// The comb and all-pass lengths (samples, mutually prime), laid end to end in the one buffer.
static const unsigned k_rv_len[4] = {1361, 1699, 557, 211}, k_rv_base[4] = {0, 1361, 3060, 3617};
static inline int32_t reverb_read(tess_sound_t *s, int i) { return s->reverb[k_rv_base[i] + s->reverb_position[i]]; }
static inline void reverb_write(tess_sound_t *s, int i, int32_t x) {
    s->reverb[k_rv_base[i] + s->reverb_position[i]] = (int16_t)(x > 30000 ? 30000 : x < -30000 ? -30000 : x);
    if (++s->reverb_position[i] == k_rv_len[i]) s->reverb_position[i] = 0;
}

static inline bool mix_active_voices(tess_sound_t *sound, uint32_t clock, int32_t *dry, int32_t *bright) {
    while (sound->pending_head < sound->pending_count && (int32_t)(clock - sound->pending[sound->pending_head].start) >= 0)
        start_note(sound, &sound->pending[sound->pending_head++]);
    if (sound->pending_head == sound->pending_count) sound->pending_head = sound->pending_count = 0;
    unsigned remaining = sound->active_voices;
    if (!remaining) return false;
    if (sound->reverb_send) sound->reverb_samples_left = sound->reverb_hold;
    for (int i = 0; i < TESS_VOICES && remaining; ++i) {
        tess_voice_t *voice = &sound->voices[i];
        if (!voice->stage) continue;
        --remaining;
        int32_t sample = voice_sample(sound, voice);
        if (voice->bright) *bright += sample;
        else *dry += sample;
    }
    return true;
}

static inline int32_t mix_reverb_sample(tess_sound_t *sound, int32_t dry) {
    int32_t input = dry * sound->reverb_send / 8;
    int32_t combs = 0;
    for (int comb = 0; comb < 2; ++comb) {
        int32_t echo = reverb_read(sound, comb);
        sound->reverb_filter_state[comb] += (echo - sound->reverb_filter_state[comb]) * REVERB_DAMPING >> 4;  // dark: the highs die first
        reverb_write(sound, comb, input + (sound->reverb_filter_state[comb] * REVERB_FEEDBACK >> 6));
        combs += echo;
    }
    int32_t output = combs / 2;
    for (int diffuser = 2; diffuser < 4; ++diffuser) {
        int32_t echo = reverb_read(sound, diffuser);
        reverb_write(sound, diffuser, output + echo / 2);
        output = echo - output / 2;
    }
    sound->reverb_filter_state[2] += (output - sound->reverb_filter_state[2]) * REVERB_WET_LOWPASS >> 4;
    int32_t wet = sound->reverb_filter_state[2] * 3 / 4;
    if (sound->reverb_samples_left < 4800) wet = wet * (int)sound->reverb_samples_left / 4800;
    if (!--sound->reverb_samples_left) {
        memset(sound->reverb, 0, sizeof sound->reverb);
        memset(sound->reverb_filter_state, 0, sizeof sound->reverb_filter_state);
    }
    return wet;
}

// The soft limiter: a slow high-pass takes the DC out, then everything above 24000 bends towards 32000.
static inline int32_t limit_sample(tess_sound_t *sound, int32_t mix) {
    sound->bass_state += (mix * 256 - sound->bass_state) >> 5;  // Q8: a plain >> 5 would leave a DC offset of ~50
    int32_t sample = (mix - ((sound->bass_state + 128) >> 8)) * 3;
    int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > 24000) {
        int32_t excess = magnitude - 24000;
        magnitude = 24000 + excess * 8000 / (8000 + excess);
        sample = sample < 0 ? -magnitude : magnitude;
    }
    return sample;
}

static inline bool mix_sample(tess_sound_t *sound, uint32_t clock, int32_t *output) {
    if (!sound->between) {  // a new value of the voices; the next sample lies halfway to the one after
        sound->dry_before = sound->dry_now;
        sound->bright_before = sound->bright_now;
        sound->dry_now = sound->bright_now = 0;
        sound->voiced = mix_active_voices(sound, clock, &sound->dry_now, &sound->bright_now);
    }
    int32_t dry = sound->between ? sound->dry_now : (sound->dry_before + sound->dry_now) / 2;
    int32_t bright = sound->between ? sound->bright_now : (sound->bright_before + sound->bright_now) / 2;
    sound->between ^= 1;
    bool reverb_active = sound->reverb_samples_left > 0;
    int32_t wet = reverb_active ? mix_reverb_sample(sound, dry) : 0;
    int32_t mix = dry + wet;
    sound->tone_state += (mix * 16 - sound->tone_state) * TONE_LOWPASS >> 8;  // Q4: a plain filter would stick a LSB or two off zero
    *output += limit_sample(sound, ((sound->tone_state + 8) >> 4) + bright);
    return sound->voiced || reverb_active;
}

bool tess_sound_mix(tess_sound_t *s, int32_t *out, int count) {
    uint32_t clock = s->gate.clock;
    s->gate.clock += (uint32_t)count;
    if (!tess_sound_active(s)) return false;
    bool audible = false;
    for (int i = 0; i < count; ++i) audible |= mix_sample(s, clock + (uint32_t)i, &out[i]);
    return audible;
}
