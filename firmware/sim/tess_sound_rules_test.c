// The rules that shape a stream of Tess sounds: rate limits (a burst never machine-guns), layering of a cue with the
// app's own sound, dropping a repeated event, the rub's escalation, the contour and register each kind of sound
// follows, the synth's cost, and what a reset keeps.
#include <stdio.h>
#include "tess_sound_names.h"
#include "tess_sound_probe.h"
#include "../main/tess_sound_rules.h"

void check_cost(tess_sound_t *sound);

static tess_sound_t s_rules;
static int32_t s_frame[PROBE_FRAME], s_pcm[PROBE_MAX_SAMPLES];

static void advance_ms(int ms) {
    for (int done = 0; done < ms; done += 20) { memset(s_frame, 0, sizeof s_frame); tess_sound_mix(&s_rules, s_frame, PROBE_FRAME); }
}
// A burst of one cue every 10 ms for a second: how many of them sounded, and the most voices at once.
static int burst_starts(tess_cue_t cue, int *peak_voices) {
    int starts = 0;
    probe_rest(&s_rules);
    for (int i = 0; i < 100; i++) {
        starts += tess_sound_cue(&s_rules, cue, .5f, 0);
        int32_t chunk[240] = {0};
        tess_sound_mix(&s_rules, chunk, 240);
        for (int j = 0; j < 240; j++) assert(abs(chunk[j]) < 32000);
        if (s_rules.active_voices > *peak_voices) *peak_voices = s_rules.active_voices;
    }
    return starts;
}
static void check_bursts(void) {
    tess_sound_reset(&s_rules);
    int peak = 0, rubs = burst_starts(TC_RUB, &peak), touches = burst_starts(TC_TOUCH, &peak);
    fprintf(stderr, "burst of 100 in 1 s: %d rubs, %d touches, %d voices at most\n", rubs, touches, peak);
    assert(rubs >= 1 && rubs <= 3);       // one stage sounds every 350 ms
    assert(touches >= 1 && touches <= 8); // a tick every 140 ms at most
    assert(peak <= TESS_WORD_VOICES);
    // Different cues in turn are held to the global gap: at most one start per 90 ms.
    probe_rest(&s_rules);
    int mixed = 0;
    for (int i = 0; i < 50; i++) {
        mixed += tess_sound_cue(&s_rules, (tess_cue_t)(i % TC_COUNT), .5f, 0);
        advance_ms(20);
    }
    assert(mixed <= 1000 / 90 + 1);
    // Every sound of the kit: a second trigger inside the event's own gap is dropped, and draws no dice.
    for (int ev = 0; ev < SFX_COUNT; ev++) {
        tess_sound_reset(&s_rules);
        probe_rest(&s_rules);
        tess_sound_event(&s_rules, (sfx_t)ev, -1, 0, .5f);
        uint32_t dice = s_rules.random;
        tess_sound_event(&s_rules, (sfx_t)ev, -1, 0, .5f);
        assert((s_rules.random == dice) == (k_event_rules[ev].gap_ms > 0));
    }
}

// One gesture is one sound: a touch cue and the app's tap layer, and a quieter cue never talks over a louder one.
static bool releasing(void) {
    for (int i = 0; i < TESS_VOICES; i++) if (s_rules.voices[i].stage == 3) return true;
    return false;
}
static void check_layering(void) {
    tess_sound_reset(&s_rules);
    probe_rest(&s_rules);
    assert(tess_sound_cue(&s_rules, TC_TOUCH, 0, 0));
    advance_ms(20);
    assert(s_rules.active_voices == 1);
    tess_sound_event(&s_rules, SFX_TAP, -1, 0, .5f);
    assert(!releasing());  // the tick still rings: the tap joined it instead of releasing it
    probe_rest(&s_rules);
    tess_sound_event(&s_rules, SFX_HELLO, -1, 0, .5f);
    advance_ms(200);
    tess_sound_event(&s_rules, SFX_TAP, -1, 0, .5f);
    advance_ms(20);
    assert(releasing());   // two separate sounds do not pile up: the first lets go
    probe_rest(&s_rules);
    assert(tess_sound_cue(&s_rules, TC_STARTLE, .8f, 0));
    advance_ms(20);
    assert(!tess_sound_cue(&s_rules, TC_GLANCE, 0, 0));    // a whisper does not talk over a reaction
    advance_ms(200);
    assert(tess_sound_cue(&s_rules, TC_GLANCE, 0, 0));     // once the gesture is over it may
    probe_rest(&s_rules);
    assert(tess_sound_cue(&s_rules, TC_TOUCH, 0, 0));
    advance_ms(100);
    assert(tess_sound_cue(&s_rules, TC_STARTLE, .8f, 0));  // the higher rank does speak over the lower
    assert(!tess_sound_cue(&s_rules, TC_COUNT, 0, 0));     // a cue that does not exist is silent, not a crash
}

// The rub: each stage is richer and higher than the one before (or as high, within 2 %), louder on average (the dice move a play by about
// 1 dB), and stage 4 hands over to TC_LOVE.
static void check_rub(void) {
    double previous = -99, previous_centroid = 0;
    for (int stage = 1; stage <= 4; stage++) {
        double total = 0, centroid = 0;
        int notes = 0;
        for (int play = 0; play < 8; play++) {
            tess_sound_reset(&s_rules);
            tess_sound_seed(&s_rules, 300 + play * 41 + stage);
            probe_rest(&s_rules);
            assert(tess_sound_cue(&s_rules, TC_RUB, stage / 4.f, 0));
            notes = s_rules.pending_count;
            probe_t p = {s_pcm, 0, 0};
            memset(s_pcm, 0, sizeof s_pcm);
            probe_finish(&s_rules, &p);
            total += probe_loudest_100ms(&p) / 8;
            centroid += probe_centroid(&p) / 8;
        }
        fprintf(stderr, "rub stage %d: %.1f dBFS, %.0f Hz, %d notes queued\n", stage, total, centroid, notes);
        assert(total > previous + .3 && centroid > previous_centroid * .98);  // (stages 2 and 3 are both about 1.7 kHz: the dice move the average by a few tens of Hz)
        previous = total;
        previous_centroid = centroid;
    }
    tess_sound_reset(&s_rules);
    probe_rest(&s_rules);
    tess_sound_cue(&s_rules, TC_RUB, 1, 0);
    advance_ms(600);
    assert(tess_sound_cue(&s_rules, TC_LOVE, .5f, 0));  // the chord reward softly replaces the last stage
    int syllables = s_rules.pending_count - s_rules.pending_head;
    assert(syllables >= 5 && syllables <= 6); // lead syllables plus two chord voices at strength .5
}

// The run of notes a trigger queued, in the order they begin. The composer adds colours to it: a low drone (the one
// held, sustained note; it begins with the run) and a high halo (the last to begin, the "sparkle"); both are left out
// when asked.
static int queued_run(const tess_sound_t *s, uint32_t *step) {
    int count = 0;
    for (int i = s->pending_head; i < s->pending_count; i++)
        if (!s->pending[i].harmony) step[count++] = s->pending[i].step;
    return count;
}
static bool is_sorted(const uint32_t *step, int n, int direction) {
    for (int i = 1; i < n; i++) if (direction * ((int64_t)step[i] - step[i - 1]) < -(int64_t)(step[i] >> 7)) return false;
    return true;
}
// Emotional words retain their final direction but turn inside; love stays on its triad.
static void check_contours(void) {
    int turns[3] = {0};
    for (int play = 0; play < 10; play++) {
        uint32_t step[TESS_PENDING];
        tess_sound_reset(&s_rules);
        tess_sound_seed(&s_rules, 900 + play);
        probe_rest(&s_rules);
        tess_sound_cue(&s_rules, TC_JOY, .5f, 0);
        int n = queued_run(&s_rules, step);
        assert(n >= 3 && step[n - 1] > step[0]);
        turns[0] += !is_sorted(step, n, 1);
        probe_rest(&s_rules);
        tess_sound_cue(&s_rules, TC_SAD, .5f, 0);
        n = queued_run(&s_rules, step);
        assert(n >= 3 && step[n - 1] < step[0]);
        turns[1] += !is_sorted(step, n, -1);
        probe_rest(&s_rules);
        tess_sound_cue(&s_rules, TC_LOVE, .5f, 0);
        n = queued_run(&s_rules, step);
        assert(n >= 3 && step[n - 1] > step[0]);
        turns[2] += !is_sorted(step, n, 1);
        for (int i = 0; i < n; i++) {  // the heart never leaves D F# A: no note is E or B
            double hz = step[i] * (double)AUDIO_RATE / 4294967296.;
            while (hz > 262) hz /= 2;
            while (hz < 130) hz *= 2;
            assert(fabs(hz - 164.81) > 6 && fabs(hz - 246.94) > 6);
        }
    }
    for (int i = 0; i < 3; i++) assert(turns[i] > 0);
}

// A touch is a quick soft swell (a few ms to half, there within 40 ms), a pair with the tap that follows.
static void check_touch(void) {
    tess_sound_reset(&s_rules);
    probe_rest(&s_rules);
    probe_t p = probe_cue(&s_rules, s_pcm, TC_TOUCH, 0, 0);
    long rise = probe_rise(&p, .5);
    fprintf(stderr, "touch: %ld ms to half its peak, sounds %d ms\n", rise * 1000 / AUDIO_RATE, probe_sounding_ms(&p));
    assert(rise * 1000 / AUDIO_RATE >= 4 && rise * 1000 / AUDIO_RATE <= 40);  // a soft swell, there within 40 ms
    assert(probe_sounding_ms(&p) <= 250);
}

// A reset silences everything but forgets no rate limit; the microphone opening cuts the tail short.
static void check_reset(void) {
    tess_sound_reset(&s_rules);
    probe_rest(&s_rules);
    assert(tess_sound_cue(&s_rules, TC_RUB, .5f, 0));
    tess_sound_reset(&s_rules);
    assert(!tess_sound_active(&s_rules));
    assert(!tess_sound_cue(&s_rules, TC_RUB, .5f, 0));
    probe_rest(&s_rules);
    tess_sound_event(&s_rules, SFX_LISTEN_START, -1, 0, .5f);
    probe_t p = {s_pcm, 0, 0};
    memset(s_pcm, 0, sizeof s_pcm);
    probe_finish(&s_rules, &p);
    fprintf(stderr, "listen_start ends after %ld ms\n", p.length * 1000 / AUDIO_RATE);
    assert(p.length * 1000 / AUDIO_RATE <= 1500);
}

// The feelings are not machine-gunned: the same one may come again only after its own gap, and a small cue never
// talks over a bigger one. The shimmer's tinkle is sparse.
static void check_feelings(void) {
    const struct { tess_cue_t cue; int gap_ms; } feelings[] = {{TC_LOVE, 10000}, {TC_JOY, 6000}, {TC_SAD, 8000}, {TC_TINKLE, 7000}};
    for (int i = 0; i < 4; i++) {
        tess_sound_reset(&s_rules);
        probe_rest(&s_rules);
        assert(tess_sound_cue(&s_rules, feelings[i].cue, 1, 0));
        advance_ms(feelings[i].gap_ms - 500);
        assert(!tess_sound_cue(&s_rules, feelings[i].cue, 1, 0));
        advance_ms(600);
        assert(tess_sound_cue(&s_rules, feelings[i].cue, 1, 0));
    }
    tess_sound_reset(&s_rules);
    probe_rest(&s_rules);
    tess_sound_event(&s_rules, SFX_DISCONNECT, -1, 0, .5f);
    advance_ms(30);
    assert(!tess_sound_cue(&s_rules, TC_SAD, 1, 0));  // the app's chime is the louder word
    advance_ms(400);
    assert(!tess_sound_cue(&s_rules, TC_SAD, 1, 0)); // do not talk over the remaining syllables
    advance_ms(2000);
    assert(tess_sound_cue(&s_rules, TC_SAD, 1, 0));
}

// The words of the moods (tess_feel.c): each is its own kind of sound, varies play to play, keeps to its own gap, and
// yields to a rub or a reaction in the same moment. Measured as the device plays them, at the strength the mood gives.
static const struct { tess_cue_t cue; int gap_ms; float strength; } k_mood_cues[] = {
    {TC_CURIOUS, 12000, .6f}, {TC_PLAYFUL, 6000, .6f}, {TC_SCARED, 4000, .6f}, {TC_GRUMPY, 8000, .6f},
    {TC_LONELY, 12000, 1}, {TC_SETTLE, 15000, .3f}};
#define MOOD_CUES ((int)(sizeof k_mood_cues / sizeof k_mood_cues[0]))
// Eight plays of the cue: the average spectral centroid and sounding length, and how many different first notes.
static void mood_profile(tess_cue_t cue, float strength, double *centroid, double *loudest, int *sounding_ms, int *first_notes) {
    uint32_t first[8];
    *centroid = *loudest = 0;
    *sounding_ms = *first_notes = 0;
    for (int play = 0; play < 8; play++) {
        tess_sound_reset(&s_rules);
        tess_sound_seed(&s_rules, 1100 + play * 37);
        probe_rest_long(&s_rules);
        assert(tess_sound_cue(&s_rules, cue, strength, 0));
        first[play] = s_rules.pending[s_rules.pending_head].step;
        bool seen = false;
        for (int before = 0; before < play; before++) seen |= first[before] < first[play] + first[play] / 100 && first[play] < first[before] + first[before] / 100;
        *first_notes += !seen;
        probe_t p = {s_pcm, 0, 0};
        memset(s_pcm, 0, sizeof s_pcm);
        probe_finish(&s_rules, &p);
        *centroid += probe_centroid(&p) / 8;
        *loudest += probe_loudest_100ms(&p) / 8;
        if (probe_sounding_ms(&p) > *sounding_ms) *sounding_ms = probe_sounding_ms(&p);
    }
}
static void check_mood_contours(void) {
    // contours: curious asks (rises); the lonely bell and the settling sigh fall
    const struct { tess_cue_t cue; int direction; } contours[] = {{TC_CURIOUS, 1}, {TC_LONELY, -1}, {TC_SETTLE, -1}};
    for (unsigned i = 0; i < sizeof contours / sizeof contours[0]; i++)
        for (int play = 0; play < 8; play++) {
            uint32_t step[TESS_PENDING];
            tess_sound_reset(&s_rules);
            tess_sound_seed(&s_rules, 1300 + play);
            probe_rest_long(&s_rules);
            assert(tess_sound_cue(&s_rules, contours[i].cue, 1, 0));
            int n = queued_run(&s_rules, step);
            assert(n >= 2 && (k_cue_rules[contours[i].cue].flags & UTTERANCE
                ? contours[i].direction * ((int64_t)step[n - 1] - step[0]) >= 0
                : is_sorted(step, n, contours[i].direction)));
        }
}
static void check_mood_gaps(void) {
    // each keeps to its own gap, and a cue that came just before (or a rub that is still ringing) shuts it out
    for (int i = 0; i < MOOD_CUES; i++) {
        tess_sound_reset(&s_rules);
        probe_rest_long(&s_rules);
        assert(tess_sound_cue(&s_rules, k_mood_cues[i].cue, 1, 0));
        advance_ms(k_mood_cues[i].gap_ms - 500);
        assert(!tess_sound_cue(&s_rules, k_mood_cues[i].cue, 1, 0));
        advance_ms(600);
        assert(tess_sound_cue(&s_rules, k_mood_cues[i].cue, 1, 0));
        int peak = 0, starts = burst_starts(k_mood_cues[i].cue, &peak);  // a storm of them: no more than the gap allows
        assert(starts <= 1 && peak <= TESS_WORD_VOICES);
        tess_sound_reset(&s_rules);
        probe_rest_long(&s_rules);
        assert(tess_sound_cue(&s_rules, TC_RUB, .5f, 0));
        advance_ms(100);
        assert(!tess_sound_cue(&s_rules, k_mood_cues[i].cue, 1, 0));
    }
}
static void check_moods(void) {
    assert(sizeof k_cue_rules <= 24 * sizeof(rule_t));  // a rule of each cue, in flash: the table does not grow past this
    double centroid[TC_COUNT], loudest[TC_COUNT];
    int sounding[TC_COUNT], first_notes[TC_COUNT];
    for (int i = 0; i < MOOD_CUES; i++) {
        tess_cue_t cue = k_mood_cues[i].cue;
        const rule_t *rule = &k_cue_rules[cue];
        for (int other = 0; other < TC_COUNT; other++)  // its own voice: no other cue has the same timbre, contour and notes
            assert(other == (int)cue || rule->timbre != k_cue_rules[other].timbre || rule->contour != k_cue_rules[other].contour || rule->mask != k_cue_rules[other].mask);
        mood_profile(cue, k_mood_cues[i].strength, &centroid[cue], &loudest[cue], &sounding[cue], &first_notes[cue]);
        fprintf(stderr, "mood cue %-8s %6.0f Hz, %5.1f dBFS, %4d ms, %d first notes in 8 plays\n", k_cue_names[cue], centroid[cue], loudest[cue], sounding[cue], first_notes[cue]);
        assert(first_notes[cue] >= 2);  // it does not play the same note twice running
    }
    // what each sounds like
    for (int i = 0; i < MOOD_CUES; i++)
        if (k_mood_cues[i].cue != TC_GRUMPY && k_mood_cues[i].cue != TC_SETTLE) assert(centroid[TC_GRUMPY] < centroid[k_mood_cues[i].cue]);  // the growl is the lowest of the awake ones (the yawn and the sigh sit as low)
    assert(centroid[TC_SCARED] > 300 && sounding[TC_SCARED] <= 1800);   // a short cluster of two or three close pads
    assert(sounding[TC_LONELY] > sounding[TC_SCARED] + 800);            // a long distant call, well over the scared cluster
    assert(loudest[TC_SETTLE] < loudest[TC_CONTENT] - 6);               // a sigh, well under the contented hum
    check_mood_contours();
    check_mood_gaps();
}

static void check_prime_variation(void) {
    const unsigned period[5] = {251, 257, 263, 269, 271};
    tess_sound_t other = {0};
    tess_sound_seed(&s_rules, 37);
    tess_sound_seed(&other, 37);
    uint16_t first[5];
    memcpy(first, s_rules.variation, sizeof first);
    for (unsigned draw = 1; draw <= 10000; draw++) {
        assert(tess_sound_random(&s_rules) == tess_sound_random(&other));
        for (int i = 0; i < 5; i++)
            assert(s_rules.variation[i] == (first[i] + draw) % period[i]);
        if (draw % 31 == 0) tess_sound_reset(&s_rules);
    }
    // Quiet resets never restart the sequence; a fresh boot seed selects new phases.
    tess_sound_seed(&other, 41);
    assert(memcmp(first, other.variation, sizeof first) != 0);
}

static void check_word_notes(tess_sound_t *word, uint16_t *vowels, const rule_t *rule) {
    for (int i = 0; i < word->pending_count; i++) {
        tess_voice_t *v = &word->pending[i];
        uint16_t vowel = (uint16_t)(((uint64_t)v->mod_step * 256 + v->step / 2) / v->step);
        if (!vowels[i]) vowels[i] = vowel;
        assert(vowel == vowels[i] && v->sustain > 0);
        if (i) assert(vowel != vowels[i - 1]);
        if (i + 1 < word->pending_count && rule->harmony != H_WARM && rule->harmony != H_SOMBER)
            assert(2 * v->hold_left < word->pending[i + 1].start - v->start);
    }
    assert(word->pending[word->pending_count - 1].hold_left > word->pending[0].hold_left);
}
static void check_word_variants(const rule_t *rule, int id, bool cue) {
    static tess_sound_t word;
    assert(!(rule->flags & (CAPTURE | HALO)));
    uint16_t vowels[4] = {0};
    for (int seed = 1; seed <= 16; seed++) {
        memset(&word, 0, sizeof word); tess_sound_reset(&word); tess_sound_seed(&word, seed);
        if (cue) assert(tess_sound_cue(&word, (tess_cue_t)id, .8f, 0));
        else tess_sound_event(&word, (sfx_t)id, -1, 0, .5f);
        int layers = 0;
        tess_sound_t lead = {0};
        for (int i = 0; i < word.pending_count; i++) {
            tess_voice_t v = word.pending[i];
            if (v.harmony) {
                layers++;
                assert(v.sustain > 0 && v.hold_left < AUDIO_RATE);
                assert(probe_voice_end(&v) <= word.gate.layer_until);
            } else lead.pending[lead.pending_count++] = v;
        }
        assert(layers == (rule->harmony ? (cue ? 3 : 2) : 0));
        assert(lead.pending_count >= 2 && lead.pending_count <= 4);
        assert(word.pending_count <= 7);
        assert(word.gate.layer_until > AUDIO_RATE / 8 && word.gate.layer_until <= 2 * AUDIO_RATE);
        check_word_notes(&lead, vowels, rule);
    }
}
static void check_chord_preemption(void) {
    static tess_sound_t word;
    for (int protected = 0; protected <= 1; protected++) {
        memset(&word, 0, sizeof word); tess_sound_reset(&word);
        // Fill the bank with held notes, then let a higher-priority word release it.
        for (int i = 0; i < TESS_WORD_VOICES; i++)
            tess_sound_note(&word, &(tess_note_t){.step = 30000000, .end_step = 30000000,
                .ratio_q8 = 256, .attack_ms = 20, .decay_ms = 20, .hold_ms = 500,
                .release_ms = 30, .sustain = 22000, .gain = 1000});
        probe_silence(&word, 2);
        assert(word.active_voices == TESS_WORD_VOICES);
        word.gate.layer_until = word.gate.clock + (protected ? AUDIO_RATE : 0);
        word.gate.last_rank = 1;
        assert(tess_sound_cue(&word, TC_JOY, .9f, 0));
        uint32_t lead = word.pending[0].step;
        int32_t frame[2] = {0}; tess_sound_mix(&word, frame, 2);
        int leads = 0, anchors = 0;
        for (int i = 0; i < TESS_VOICES; i++) {
            tess_voice_t *v = &word.voices[i];
            if (v->stage == 3 || !v->stage) continue;
            leads += !v->harmony && v->step == lead;
            anchors += v->harmony;
        }
        assert(leads == 1); // delayed accompaniment cannot steal the newly started lead
        assert(anchors == 1);
    }
}
static void check_harmony(void) {
    static tess_sound_t word;
    const tess_cue_t cues[] = {TC_LOVE, TC_JOY, TC_SAD, TC_SCARED, TC_GRUMPY, TC_LONELY, TC_CONTENT};
    for (unsigned cue = 0; cue < sizeof cues / sizeof cues[0]; cue++)
    for (int strong = 0; strong <= 1; strong++) {
        memset(&word, 0, sizeof word); tess_sound_reset(&word); tess_sound_seed(&word, 37);
        assert(tess_sound_cue(&word, cues[cue], strong ? .9f : .3f, 0));
        int layers = 0, rising = 0, falling = 0;
        uint32_t first_end = 0, early_end = 0;
        for (int i = 0; i < word.pending_count; i++) {
            tess_voice_t *v = &word.pending[i];
            if (!v->harmony) continue;
            uint32_t end = probe_voice_end(v);
            if (!layers) first_end = end;
            else if (layers == 1) early_end = end;
            rising += v->glide > 0; falling += v->glide < 0; layers++;
        }
        assert(layers == 2 + strong && rising + falling >= 1);
        assert(early_end != first_end); // emotion-specific voices have independent lifetimes
        while (tess_sound_active(&word)) {
            int32_t frame[PROBE_FRAME] = {0}; tess_sound_mix(&word, frame, PROBE_FRAME);
            assert(word.active_voices < TESS_WORD_VOICES); // leaves space for a joined control tone
        }
    }
    memset(&word, 0, sizeof word); tess_sound_reset(&word);
    assert(tess_sound_cue(&word, TC_SCARED, .9f, 0));
    probe_silence(&word, 5);
    assert(tess_sound_cue(&word, TC_JOY, .9f, 0));
    assert(word.pending_head == 0 && word.pending_count <= 7);
    probe_silence(&word, 5);
    assert(tess_sound_cue(&word, TC_LOVE, .9f, 0));
    assert(word.pending_head == 0 && word.pending_count <= 7);
    assert(!k_cue_rules[TC_CURIOUS].harmony);
    assert(!k_event_rules[SFX_TAP].harmony && !(k_event_rules[SFX_TAP].flags & UTTERANCE));
}
static void check_language(void) {
    static tess_sound_t word;
    for (int kind = 0; kind < SFX_COUNT + TC_COUNT; kind++) {
        bool cue = kind >= SFX_COUNT;
        int id = cue ? kind - SFX_COUNT : kind;
        const rule_t *rule = cue ? &k_cue_rules[id] : &k_event_rules[id];
        assert((cue ? tess_sound_cue_is_utterance((tess_cue_t)id) : tess_sound_event_is_utterance((sfx_t)id)) == !!(rule->flags & UTTERANCE));
        if (rule->flags & UTTERANCE) check_word_variants(rule, id, cue);
    }
    memset(&word, 0, sizeof word); tess_sound_reset(&word);
    assert(tess_sound_cue(&word, TC_LOVE, .8f, 0));
    int count = word.pending_count;
    probe_silence(&word, 10); // 200 ms: still in the middle of the word, past the old 120 ms window.
    assert(!tess_sound_cue(&word, TC_GLANCE, 0, 0));
    assert(word.pending_count == count);
    uint32_t until = word.gate.layer_until;
    tess_sound_event(&word, SFX_TAP, -1, 0, .5f); // joined feedback cannot shorten the word
    assert(word.gate.layer_until >= until);
    probe_silence(&word, 10);
    assert(!tess_sound_cue(&word, TC_GLANCE, 0, 0));
    tess_sound_event(&word, SFX_LISTEN_START, -1, 0, .5f);
    assert(word.pending_count == k_event_rules[SFX_LISTEN_START].notes_min);
    assert(!tess_sound_cue_is_utterance(TC_COUNT) && !tess_sound_event_is_utterance(SFX_COUNT));
}

void check_profiles(void);
void check_rules(void) {
    check_profiles();
    check_chord_preemption();
    check_harmony();
    check_language();
    check_prime_variation();
    check_bursts();
    check_layering();
    check_rub();
    check_contours();
    check_touch();
    check_reset();
    check_feelings();
    check_moods();
    check_cost(&s_rules);
}
