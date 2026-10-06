#include <math.h>
#include "tess_sound_rules.h"

#define CUE_GAP_MS 90        // no two cues start closer than this
#define LAYER_MS 120         // a sound this fresh is still the same gesture: cues below it stay out, events join it
#define BRIGHT_SPAN_Q8 51    // how much the height of a touch (energy) deepens the swell's FM (.2)
#define MAX_INDEX_Q8 384     // 1.5 radians: the deepest FM the synth takes (a low index keeps every sideband low)
#define MUFFLE_FROM A3       // above this degree the FM depth falls (a high note's sidebands would be high partials)
#define MUFFLE_PERCENT 6     // per degree
#define SETTLE_MS 8          // a sustained note falls to its level this fast
#define LANDING_PERCENT 140  // the last note of a run rings longer
#define HALO_GAIN_DIV 4      // the slow swell after a run: its level under the run's
#define HALO_ATTACK_MS 160
#define HALO_DECAY_MS 250
#define HALO_DETUNE 5
#define HALO_SHARE 12000
#define TIME_CONSTANTS 4     // a note is 35 dB down after this many time constants: how its length is taken
#define DETUNE_UNITS 3       // a note may be this many 1/1024 of its pitch (about 1.7 cents each) off

static int clamp_int(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
static bool pending(uint32_t clock, uint32_t free_at) { return (int32_t)(clock - free_at) < 0; }
static int random_between(tess_sound_t *s, int low, int high) { return low + (int)(tess_sound_random(s) % (unsigned)(high - low + 1)); }
static int32_t samples_from_ms(int ms) { return ms * AUDIO_RATE / 1000; }

// D3 E3 F#3 A3 B3 as phase steps; every octave above doubles them.
#define STEP_OF(hz) ((uint32_t)((hz) * 4294967296.0 / AUDIO_RATE + .5))
static uint32_t degree_step(int degree) {
    static const uint32_t k_scale_step[5] = {STEP_OF(146.83), STEP_OF(164.81), STEP_OF(185.0), STEP_OF(220.0), STEP_OF(246.94)};
    degree = clamp_int(degree, 0, SCALE_DEGREES - 1);
    return k_scale_step[degree % 5] << (degree / 5);
}

// The degree `steps` notes of the mask above `start` (start itself moved up to the first note the mask has).
static int ladder_degree(uint8_t mask, int start, int steps) {
    int degree = start;
    while (!(mask >> (degree % 5) & 1)) degree++;
    while (steps-- > 0) do degree++; while (!(mask >> (degree % 5) & 1));
    return degree;
}

// How far up the ladder note i of n stands.
static int contour_step(int contour, int i, int n, int span) {
    if (n < 2) return 0;
    switch (contour) {
    case RISE: return i * span / (n - 1);
    case FALL: return (n - 1 - i) * span / (n - 1);
    case ARCH: return (i < n - 1 - i ? i : n - 1 - i) * 2 * span / (n - 1);
    case ZIGZAG: return i % 2 * span;
    default: return 0;
    }
}

// Choose a whole melodic pronunciation once: turns and returns replace an
// obligatory staircase, while the rule retains its register and final direction.
static int word_step(const rule_t *rule, int i, int count, int variant) {
    static const uint8_t paths[2][3][4] = {
        {{0, 3, 2, 0}, {1, 0, 3, 0}, {0, 2, 1, 0}},
        {{0, 3, 1, 3}, {1, 0, 3, 2}, {0, 2, 1, 3}}};
    if (count < 3 || (rule->contour != RISE && rule->contour != FALL))
        return contour_step(rule->contour, i, count, rule->span);
    int step = (paths[count == 4][variant][i] * rule->span + 1) / 3;
    if (i == count - 1 && rule->span) {
        int first = (paths[count == 4][variant][0] * rule->span + 1) / 3;
        step = clamp_int(step, first + 1, rule->span);
    }
    return rule->contour == FALL ? rule->span - step : step;
}

// What one play does to its rule: the level and the strike's brightness (Q8).
typedef struct { int level, brightness, jitter; } play_t;

// One note of the run, drawn afresh: its pitch slightly off, its length, swell, level and ensemble each within a range.
static tess_note_t compose_note(tess_sound_t *s, const rule_t *rule, const play_t *play, int degree, int at_ms, bool landing) {
    const timbre_t *t = &k_timbres[rule->timbre];
    int detune = random_between(s, -DETUNE_UNITS, DETUNE_UNITS), side = tess_sound_random(s) & 1 ? 1 : -1;
    uint32_t step = degree_step(degree), end = degree_step(clamp_int(degree + rule->bend, 0, MAX_DEGREE));
    int length = rule->decay_ms * random_between(s, 80, 125) / 100 * (landing ? LANDING_PERCENT : 100) / 100;
    bool sustained = t->sustain > 0;
    int muffle = 100 - MUFFLE_PERCENT * (degree > MUFFLE_FROM ? degree - MUFFLE_FROM : 0);
    // Long character phrases alternate hollow and woody harmonic resonances.
    // Whole-number partials preserve the note; quick controls retain their timbre.
    bool resonant = rule->timbre == PAD || rule->timbre == HUM;
    int harmonic = resonant ? random_between(s, 2, 3) : 1;
    return (tess_note_t){
        .delay_samples = (uint32_t)samples_from_ms(at_ms),
        .step = step + (step >> 10) * (uint32_t)detune, .end_step = end + (end >> 10) * (uint32_t)detune,
        .ratio_q8 = (uint16_t)(t->ratio_q8 * harmonic), .layer_ratio_q8 = t->layer_ratio_q8,
        .chorus_detune = (int16_t)(side * t->chorus_detune * random_between(s, 85, 115) / 100),
        .index_q8 = (uint16_t)clamp_int(t->index_q8 * play->brightness / 256 * random_between(s, 75, 130) / 100 * clamp_int(muffle, 25, 100) / 100, 0, MAX_INDEX_Q8),
        .attack_ms = (uint16_t)(t->attack_ms * random_between(s, 85, 130) / 100), .decay_ms = (uint16_t)(sustained ? SETTLE_MS : length / TIME_CONSTANTS),
        .ring_ms = t->ring_ms, .hold_ms = (uint16_t)(sustained ? length : 0), .release_ms = t->release_ms,
        .glide_ms = rule->bend ? (uint16_t)length : 0,
        .gain = (uint16_t)(rule->gain * play->level / 256 * random_between(s, 100 - play->jitter, 100 + play->jitter) / 100), .chorus_gain = t->chorus_gain, .layer_gain = t->layer_gain,
        .sustain = t->sustain};
}

// Where on the scale: by the side of the touch, by the strength, and a random step or two (not the last one again).
static int pick_root(tess_sound_t *s, const rule_t *rule, float position, float strength) {
    int root = rule->low + (int)lrintf(rule->pan * position) + (int)lrintf(rule->climb * strength);
    if (rule->vary) {
        int extra = (int)(tess_sound_random(s) % (unsigned)(rule->vary + 1));
        if (extra == s->gate.last_root) extra = (extra + 1) % (rule->vary + 1);
        s->gate.last_root = (uint8_t)extra;
        root += extra;
    }
    return clamp_int(root, 0, MAX_DEGREE);
}

static int pick_count(tess_sound_t *s, const rule_t *rule, float strength) {
    if (rule->flags & BY_STRENGTH) return clamp_int((int)(strength * rule->notes_max), rule->notes_min, rule->notes_max);
    return random_between(s, rule->notes_min, rule->notes_max);
}

// A rule is a word; its contour and vowel order stay recognizable across pronunciations.
// Short release separates syllables without adding any work to the sample loop.
static void blend_word(tess_note_t *note, const rule_t *rule, int spacing, bool last) {
    if (rule->harmony == H_WARM || rule->harmony == H_SOMBER) {
        note->attack_ms = 50;
        note->hold_ms = (uint16_t)(spacing * (last ? 105 : 80) / 100);
        note->release_ms = last ? 48 : 30;
        note->index_q8 = 80;
    }
}

static void articulate(tess_note_t *note, const rule_t *rule, int syllable, bool last) {
    static const uint16_t vowels[3] = {256, 768, 512};
    int spacing = (rule->spacing_min + rule->spacing_max) / 2;
    if (!spacing) spacing = clamp_int(rule->decay_ms / 6, 30, 90);
    note->ratio_q8 = vowels[(rule->timbre + rule->contour + syllable) % 3];
    note->attack_ms = 24 + note->attack_ms % 13;
    note->decay_ms = 18;
    note->sustain = 22000;
    note->hold_ms = (uint16_t)(spacing * (last ? 90 : 55) / 100);
    note->release_ms = last ? 48 : 16;
    if (!(rule->flags & UTTERANCE) && !rule->harmony) {
        note->release_ms = last ? 20 : 12;
        note->hold_ms = (uint16_t)(spacing * 55 / 100);
    }
    note->index_q8 = 160 + note->index_q8 % 65;
    note->ring_ms = 45;
    note->glide_ms = rule->bend ? note->hold_ms : 0;
    note->gain = (uint16_t)(note->gain * (last ? 120 : 110) / 100);
    if (rule->harmony) note->gain = note->gain * 90 / 100;
    blend_word(note, rule, spacing, last);
}

// The colours around a run: a slow pure swell an octave above the last note that blooms after it, and a soft low note
// under the whole run (its harmonics carry it: the small speaker plays nothing under 200 Hz).
static void add_colours(tess_sound_t *s, const rule_t *rule, const play_t *play, int last_degree, int last_at_ms) {
    int level = rule->gain * play->level / 256;
    if (rule->flags & HALO) {
        int up = last_degree + 5 > MAX_DEGREE ? last_degree : last_degree + 5;  // (never over the register: then it is the note's own pitch)
        uint32_t step = degree_step(up);
        tess_sound_note(s, &(tess_note_t){.delay_samples = (uint32_t)samples_from_ms(last_at_ms + 40), .step = step, .end_step = step,
                            .ratio_q8 = 256, .attack_ms = HALO_ATTACK_MS, .decay_ms = HALO_DECAY_MS, .chorus_detune = HALO_DETUNE,
                            .chorus_gain = HALO_SHARE, .gain = (uint16_t)(level / HALO_GAIN_DIV)});
    }

}

// Emotion-specific arrangements: intervals, entries and lengths are independent.
// No obligatory bass. Warm/somber words blend; bright/tense words articulate.
typedef struct {
    uint8_t from[3], to[3], entry[3], length[3], gain[3];
    uint16_t attack, release;
} harmony_t;
static const harmony_t k_harmony[H_COUNT] = {
    [H_WARM]   = {{0,2,1}, {1,1,3}, {0,8,32},  {72,56,40}, {28,23,18}, 100,35},
    [H_BRIGHT] = {{1,3,2}, {3,1,4}, {0,18,40}, {48,42,30}, {25,23,20}, 60,24},
    [H_SOMBER] = {{2,1,3}, {0,0,1}, {0,12,28}, {70,58,45}, {28,22,17}, 110,35},
    [H_TENSE]  = {{0,2,1}, {3,0,2}, {0,8,24},  {45,60,48}, {23,24,20}, 50,20},
    [H_LOW]    = {{0,1,2}, {0,0,1}, {0,22,38}, {55,42,30}, {24,21,18}, 75,24},
    [H_AIRY]   = {{1,3,2}, {0,1,0}, {12,32,52},{48,35,20}, {22,20,18}, 130,35},
};

static tess_note_t harmony_note(tess_sound_t *s, const rule_t *rule, const play_t *play,
                               int root, int phrase_ms, int i, int variant) {
    const harmony_t *h = &k_harmony[rule->harmony];
    int degree = ladder_degree(rule->mask, root, h->from[i] + (variant == 1));
    int end = ladder_degree(rule->mask, root, h->to[i] + (variant == 2));
    if (rule->harmony == H_WARM && i == 0 && variant == 0 && root >= 5) degree -= 5;
    int entry = h->entry[i] ? clamp_int(h->entry[i] + random_between(s, -4, 4), 0, 55) : 0;
    int delay = phrase_ms * entry / 100;
    int available = phrase_ms - delay - 6 * h->release - 30;
    int attack = clamp_int(h->attack + random_between(s, -15, 15), 15, available / 3);
    int length = clamp_int(phrase_ms * (h->length[i] + random_between(s, -5, 5)) / 100, attack + 1, available);
    return (tess_note_t){.harmony = true, .delay_samples = (uint32_t)samples_from_ms(delay),
        .step = degree_step(degree), .end_step = degree_step(end),
        .ratio_q8 = 256, .attack_ms = (uint16_t)attack, .decay_ms = 20,
        .hold_ms = (uint16_t)(length - attack), .release_ms = h->release,
        .glide_ms = (uint16_t)length, .sustain = 26000,
        .gain = (uint16_t)(rule->gain * play->level / 256 * h->gain[i] / 100)};
}

static void harmonize(tess_sound_t *s, const rule_t *rule, const play_t *play,
                      int root, int phrase_ms, float strength, int variant) {
    int voices = strength >= .65f ? 3 : 2, highest = 0;
    const harmony_t *h = &k_harmony[rule->harmony];
    for (int i = 0; i < voices; i++) {
        int from = h->from[i] + (variant == 1), to = h->to[i] + (variant == 2);
        if (from > highest) highest = from;
        if (to > highest) highest = to;
    }
    // Move the whole arrangement down together; never collapse upper intervals.
    while (root > 0 && ladder_degree(rule->mask, root, highest) > MAX_DEGREE) root = clamp_int(root - 5, 0, MAX_DEGREE);
    for (int i = 0; i < voices; i++) {
        tess_note_t note = harmony_note(s, rule, play, root, phrase_ms, i, variant);
        tess_sound_note(s, &note);
    }
}

typedef struct { int root, count, variant, degree, at_ms, duration_ms; } phrase_t;

static int phrase_gap(tess_sound_t *s, const rule_t *rule, int count, int i, int variant) {
    int gap = random_between(s, rule->spacing_min, rule->spacing_max);
    if (!(rule->flags & CAPTURE) && count >= 3) gap = gap * ((i + variant) % 2 ? 112 : 88) / 100;
    return gap;
}

static void queue_phrase(tess_sound_t *s, const rule_t *rule, const play_t *play, phrase_t *phrase) {
    for (int i = 0; i < phrase->count; i++) {
        int offset = !(rule->flags & CAPTURE) ? word_step(rule, i, phrase->count, phrase->variant)
            : contour_step(rule->contour, i, phrase->count, rule->span);
        phrase->degree = ladder_degree(rule->mask, phrase->root, offset);
        tess_note_t note = compose_note(s, rule, play, phrase->degree, phrase->at_ms, i == phrase->count - 1 && phrase->count > 1);
        if (!(rule->flags & CAPTURE)) {
            articulate(&note, rule, i, i == phrase->count - 1);
            if ((rule->flags & UTTERANCE) || rule->harmony)
                phrase->duration_ms = phrase->at_ms + note.attack_ms + note.hold_ms + 6 * note.release_ms;
        }
        if (rule->flags & CAPTURE) {
            note.ratio_q8 = i ? 768 : 256;
            note.index_q8 = 160;
            note.ring_ms = 45;
        }
        tess_sound_note(s, &note);
        if (i < phrase->count - 1) phrase->at_ms += phrase_gap(s, rule, phrase->count, i, phrase->variant);
    }
}

// All actions share the spoken palette; microphone cues retain their short capture envelope.
static int compose(tess_sound_t *s, const rule_t *rule, float position, float strength, float energy, bool interface) {
    play_t play = {256, 256 + (int)(BRIGHT_SPAN_Q8 * energy), interface ? 2 : 8};
    if (rule->flags & SCALED) play.level = (int)(256 * (.55f + .45f * strength));
    if (rule->flags & BRIGHTEN) play.brightness += (int)(256 * strength);
    phrase_t phrase = {.root = pick_root(s, rule, position, strength), .count = pick_count(s, rule, strength),
        .variant = !(rule->flags & CAPTURE) ? random_between(s, 0, 2) : 0, .duration_ms = LAYER_MS};
    int top = ladder_degree(rule->mask, phrase.root, rule->span) + (rule->bend > 0 ? rule->bend : 0);
    for (; top > MAX_DEGREE && phrase.root >= 5; top -= 5) phrase.root -= 5;
    queue_phrase(s, rule, &play, &phrase);
    if (rule->harmony) harmonize(s, rule, &play, phrase.root, phrase.duration_ms, strength, phrase.variant);
    add_colours(s, rule, &play, phrase.degree, phrase.at_ms);
    return phrase.duration_ms;
}

static void start_sound(tess_sound_t *s, const rule_t *rule, float position, float strength, float energy, bool interface) {
    s->reverb_send = rule->send;
    if (rule->send) s->reverb_hold = (unsigned)samples_from_ms(rule->hold_ms);
    if (rule->flags & CAPTURE) tess_sound_fade_reverb(s);
    int phrase_ms = compose(s, rule, position, strength, energy, interface);
    uint32_t until = s->gate.clock + (uint32_t)samples_from_ms(phrase_ms);
    if (pending(s->gate.clock, s->gate.layer_until)) {
        if (pending(until, s->gate.layer_until)) until = s->gate.layer_until;
        if (rule->rank > s->gate.last_rank) s->gate.last_rank = rule->rank;
    } else s->gate.last_rank = rule->rank;
    s->gate.layer_until = until;
}

// Offline frequency-weighted calibration; no work is added to the sample loop.
static int notch_gain(sfx_t event, int index) {
    static const uint16_t volume[] = {628, 404, 256, 183, 146};
    static const uint16_t detent[] = {961, 743, 521, 413, 336, 256, 213, 171, 152, 135, 121};
    static const uint16_t glint[] = {388, 388, 311, 256, 256, 256, 195, 158, 158, 158, 129};
    switch (event) {
    case SFX_VOLUME: return volume[clamp_int(index, 0, 4)];
    case SFX_DETENT: return detent[clamp_int(index, 0, 10)];
    case SFX_GLINT: return glint[clamp_int(index, 0, 10)];
    default: return 256;
    }
}

void tess_sound_event(tess_sound_t *s, sfx_t event, int index, float position, float energy) {
    if ((unsigned)event >= SFX_COUNT || !k_event_rules[event].notes_max) return;
    rule_t adjusted = k_event_rules[event];
    const rule_t *rule = &adjusted;
    tess_gate_t *g = &s->gate;
    if (rule->gap_ms && g->last_event == event && pending(g->clock, g->event_free_at)) return;
    // The app plays its sound a moment after the cue that began the gesture: they layer, one gesture.
    bool joins_cue = g->last_was_cue && pending(g->clock, g->layer_until) && !(rule->flags & CAPTURE) && !rule->harmony;
    if (!joins_cue) {
        tess_sound_release(s);
        g->layer_until = g->clock; // explicit new action/capture starts its own protection window
    }
    float steps = event == SFX_VOLUME ? 4.f : 10.f;  // the stepped groups: the notch is the strength
    float strength = index >= 0 ? fminf(fmaxf(index / steps, 0), 1) : .5f;
    if (index >= 0) adjusted.gain = adjusted.gain * notch_gain(event, index) / 256;
    start_sound(s, rule, fminf(fmaxf(position, -1), 1), strength, energy, true);
    g->last_event = (uint8_t)event;
    g->event_free_at = g->clock + (uint32_t)samples_from_ms(rule->gap_ms);
    g->last_was_cue = joins_cue;
}

bool tess_sound_cue_is_utterance(tess_cue_t cue) {
    return (unsigned)cue < TC_COUNT && (k_cue_rules[cue].flags & UTTERANCE);
}
bool tess_sound_event_is_utterance(sfx_t event) {
    return (unsigned)event < SFX_COUNT && (k_event_rules[event].flags & UTTERANCE);
}

// Selected Celesta high: one B minor pentatonic, compatible with the UI's D major.
// Impact speed lifts each point through two registers; no detune or pitch glide.
static void particle_note(tess_sound_t *s, float strength, float identity) {
    static const uint32_t pitches[] = {STEP_OF(739.98885), STEP_OF(880), STEP_OF(987.76660),
        STEP_OF(1174.65907), STEP_OF(1318.51023), STEP_OF(1479.97769), STEP_OF(1760),
        STEP_OF(1975.53321), STEP_OF(2349.31814), STEP_OF(2637.02046)}; // F#5..E7, B minor pentatonic
    unsigned point = (unsigned)lrintf((identity + 1) * 55.5f);
    unsigned color = (point * 73u + 19u) % 112u;
    uint32_t pitch = pitches[color % 5 + (unsigned)(strength * 5)];
    int decay = random_between(s, 85, 115);
    tess_note_t note = {.bright = true, .step = pitch, .end_step = pitch,
        .ratio_q8 = 256, .layer_ratio_q8 = 512,
        .attack_ms = 5, .decay_ms = (uint16_t)decay,
        .ring_ms = (uint16_t)(decay * 55 / 100), .release_ms = (uint16_t)decay,
        .gain = (uint16_t)(350 * strength)};
    tess_sound_note(s, &note);
    s->reverb_send = 0;
    s->reverb_hold = 0;
}

bool tess_sound_cue(tess_sound_t *s, tess_cue_t cue, float strength, float position) {
    if ((unsigned)cue >= TC_COUNT) return false;
    const rule_t *rule = &k_cue_rules[cue];
    tess_gate_t *g = &s->gate;
    if (pending(g->clock, g->cue_free_at[cue]) || pending(g->clock, g->cue_any_free_at)) return false;
    if (pending(g->clock, g->layer_until) && rule->rank <= g->last_rank) return false;
    // An accepted chord replaces old voices (including unprotected tails) with a soft release.
    // Layer voices within one word, never queue several full words over each other.
    if (rule->harmony) {
        tess_sound_release(s);
        g->layer_until = g->clock;
    }
    strength = fminf(fmaxf(strength, 0), 1);
    position = fminf(fmaxf(position, -1), 1);
    if (cue == TC_IMPACT) {
        particle_note(s, strength, position);
        return true;  // independent grains from this frame share the bounded synth
    }
    start_sound(s, rule, position, strength, strength, false);
    g->cue_free_at[cue] = g->clock + (uint32_t)samples_from_ms(rule->gap_ms);
    g->cue_any_free_at = g->clock + (uint32_t)samples_from_ms(CUE_GAP_MS);
    g->last_was_cue = true;
    return true;
}
