// Tess's composed sounds on the host: every event and cue, five times over, renders clean (bounded, no click at the
// edges, no DC), dies away, sits in its loudness class, uses only the D major pentatonic, follows the rule that drives
// it (contour, register, side of a touch), is never the same twice, and is the same for the same seed. Tiny particle
// grains have separate density, peak and duration checks in tess_sound_profiles_test.c. The rules that
// shape a stream of sounds (rate limits, layering, the rub's escalation) are in tess_sound_rules_test.c.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "tess_sound_names.h"
#include "tess_sound_probe.h"
#include "../main/tess_sound_rules.h"

// UI events: A-weighted loudest 100 ms. Character classes: unweighted RMS. The Plush kit is -19.5 in all of them (-20 LUFS); Tess is a
// touch lower and softest where it is heard most often: whisper (contact ticks) < ui < character < alert.
#define TICK -36.f  // a contact tick is a few ms long: over a whole 100 ms its energy is small, its peak is not
#define WHISPER -31.f
#define UI -25.f
#define THINK -27.f
#define CHARACTER -22.f
#define ALERT -19.5f
#define TOLERANCE 3.f  // dB; the dice (level, length, brightness) move one sound by up to about 2 dB
#define UI_PERCEIVED -42.f
// Each cue at the strength it is tested with, and its class.
static const float k_cue_strength[TC_COUNT] = {[TC_TOUCH] = 0, [TC_FLING] = .9f, [TC_SWING] = .6f, [TC_EXCITE] = .5f,
    [TC_RUB] = 1, [TC_GLANCE] = 0, [TC_PEEK] = .6f, [TC_INVITE] = .6f, [TC_DODGE] = .6f, [TC_STARTLE] = .8f,
    [TC_CONTENT] = .6f, [TC_MISCHIEF] = .6f, [TC_LOVE] = 1, [TC_JOY] = 1, [TC_SAD] = 1, [TC_TINKLE] = .5f, [TC_SNAP] = 1,
    [TC_CURIOUS] = .6f, [TC_PLAYFUL] = .6f, [TC_SCARED] = .6f, [TC_GRUMPY] = .6f, [TC_LONELY] = 1, [TC_SETTLE] = .3f, [TC_IMPACT] = .65f};
static const float k_cue_db[TC_COUNT] = {[TC_TOUCH] = TICK, [TC_FLING] = -23, [TC_SWING] = -24, [TC_EXCITE] = -25,
    [TC_RUB] = -21.5f, [TC_GLANCE] = WHISPER, [TC_PEEK] = -24, [TC_INVITE] = -24, [TC_DODGE] = -25,
    [TC_STARTLE] = CHARACTER, [TC_CONTENT] = -23, [TC_MISCHIEF] = -24,
    [TC_LOVE] = ALERT, [TC_JOY] = CHARACTER, [TC_SAD] = CHARACTER, [TC_TINKLE] = -34, [TC_SNAP] = -24,
    [TC_CURIOUS] = UI, [TC_PLAYFUL] = CHARACTER, [TC_SCARED] = CHARACTER, [TC_GRUMPY] = CHARACTER, [TC_LONELY] = UI,
    [TC_SETTLE] = WHISPER, [TC_IMPACT] = -43};
#define PLAYS 5  // triggers of each sound in a row
static int32_t s_storage[PROBE_MAX_SAMPLES];
static tess_sound_t s_sound;

// How long a sound of a class may sound (35 dB under its peak): short sounds stay short.
static int longest_sounding_ms(float db) { return db <= -30 ? 2800 : db <= -24 ? 3000 : 4000; }
static int notch_of(sfx_t event) { return event == SFX_VOLUME ? 2 : event == SFX_DETENT || event == SFX_GLINT ? 5 : -1; }

// The palette: no high ringing (under 5 % of the energy above 2.5 kHz, what the small speaker would make shrill) and an
// ensemble, never a lone sine (at least two partials standing at once). The worst of each is kept for the report.
#define HIGH_SHARE_MAX .05
// And no noise: a pad is a few sines, so the spectrum of its noisiest loud stretch is spiky, never flat (white noise is
// about .5; the filtered-noise breath this palette retired made .02 to .08; the pads are at .003).
#define FLATNESS_MAX .02
static double s_worst_high, s_worst_flatness;
static int s_slowest_quick = 0;
static int s_fewest_partials = 99;
static void check_palette(const probe_t *p, const char *name) {
    double high = probe_high_share(p, 2500), flatness = probe_flatness(p);
    int partials = probe_partials(p);
    if (high > s_worst_high) s_worst_high = high;
    if (flatness > s_worst_flatness) s_worst_flatness = flatness;
    if (partials < s_fewest_partials) s_fewest_partials = partials;
    if (high > HIGH_SHARE_MAX || partials < 2 || flatness > FLATNESS_MAX)
        fprintf(stderr, "    %s: %.2f %% above 2.5 kHz, %d partials, flatness %.4f\n", name, 100 * high, partials, flatness);
    assert(high <= HIGH_SHARE_MAX && partials >= 2 && flatness <= FLATNESS_MAX);
}

// Articulated words and controls start softly but promptly, including capture cues.
static void check_onset(const probe_t *p, const rule_t *rule, const char *name) {
    (void)rule;
    int onset = probe_onset_ms(p);
    if (onset > s_slowest_quick) s_slowest_quick = onset;
    bool valid = onset >= 8 && onset <= 60;
    if (!valid) fprintf(stderr, "    %s: onset %d ms\n", name, onset);
    assert(valid);
}

// Structural safety and character RMS classes. UI passes NAN: its frequency-weighted
// level is checked by the caller instead of the obsolete event RMS classes.
static double check_clean(const probe_t *p, float target_db) {
    int peak = probe_peak(p);
    long last = p->length - 1;
    assert(p->length > 0 && peak > 200);                 // not silent
    assert(peak < 16384);                                 // 6 dB under full scale: the limiter never works hard
    assert(abs(p->pcm[0]) <= peak / 50 + 4);              // starts from silence
    for (int i = 0; i < 24; i++) assert(abs(p->pcm[last - i]) <= 3);  // and ends in it
    assert(probe_worst_click(p) < 8);                     // no click: a hard cut of a tone scores above 20, these under 5
    long sum = 0;
    for (long i = 0; i < p->length; i++) sum += p->pcm[i];
    assert(labs(sum / p->length) < 12);                   // no DC
    double db = probe_loudest_100ms(p);
    if (fabs(db - target_db) > TOLERANCE) fprintf(stderr, "    out of class: %.1f dB (class %.1f)\n", db, target_db);
    assert(isnan(target_db) || fabs(db - target_db) <= TOLERANCE);
    if (probe_sounding_ms(p) > longest_sounding_ms(target_db)) fprintf(stderr, "    too long: %d ms (limit %d)\n", (int)probe_sounding_ms(p), longest_sounding_ms(target_db));
    assert(probe_sounding_ms(p) <= longest_sounding_ms(target_db));
    assert(p->peak_voices <= TESS_VOICES);
    return db;
}

// Every note that is queued starts on a scale note (D E F# A B in any octave, within the detune), and none is pitched
// over the register's top, at either end of its glide.
#define TOP_HZ 1000.
static void check_on_scale(const tess_sound_t *s, const char *name) {
    static const double k_hz[5] = {146.83, 164.81, 185., 220., 246.94};
    for (int i = s->pending_head; i < s->pending_count; i++) {
        double hz = s->pending[i].step * (double)AUDIO_RATE / 4294967296., best = 1e9;
        assert(hz <= TOP_HZ && s->pending[i].step * (double)AUDIO_RATE / 4294967296. > 100);
        assert(s->pending[i].step + (int64_t)s->pending[i].glide * s->pending[i].glide_left <= (uint32_t)(TOP_HZ * 4294967296. / AUDIO_RATE * 1.004));
        for (int octave = 0; octave < 5; octave++)
            for (int degree = 0; degree < 5; degree++) {
                double error = fabs(hz / (k_hz[degree] * (1 << octave)) - 1);
                if (error < best) best = error;
            }
        if (best > .004) fprintf(stderr, "%s: note %d at %.1f Hz is off the scale by %.2f %%\n", name, i, hz, 100 * best);
        assert(best <= .004);
    }
}

// Renders `PLAYS` triggers of each event in a row; their signatures must all differ, every play must be clean and in
// class, and stay on the scale.
static void check_events(void) {
    for (int ev = 0; ev < SFX_COUNT; ev++) {
        uint64_t signatures[PLAYS];
        double mean = 0;
        tess_sound_reset(&s_sound);
        for (int play = 0; play < PLAYS; play++) {
            probe_rest(&s_sound);
            tess_sound_event(&s_sound, (sfx_t)ev, notch_of((sfx_t)ev), 0, .5f);
            check_on_scale(&s_sound, k_sfx_names[ev]);
            probe_t p = {s_storage, 0, 0};
            memset(s_storage, 0, sizeof s_storage);
            probe_finish(&s_sound, &p);
            check_clean(&p, NAN);
            double perceived = probe_perceived_100ms(&p);
            if (fabs(perceived - UI_PERCEIVED) > TOLERANCE)
                fprintf(stderr, "%s perceived %.3f dB\n", k_sfx_names[ev], perceived);
            assert(fabs(perceived - UI_PERCEIVED) <= TOLERANCE);
            mean += perceived / PLAYS;
            check_palette(&p, k_sfx_names[ev]);
            check_onset(&p, &k_event_rules[ev], k_sfx_names[ev]);
            signatures[play] = probe_signature(&p);
            for (int before = 0; before < play; before++) assert(signatures[before] != signatures[play]);
        }
        fprintf(stderr, "  %-14s %6.1f dB (class %5.1f)\n", k_sfx_names[ev], mean, UI_PERCEIVED);
    }
}
static void check_cues(void) {
    for (int cue = 0; cue < TC_COUNT; cue++) {
        if (cue == TC_IMPACT) continue;  // short collision grains have their own duration/density checks
        uint64_t signatures[PLAYS];
        double mean = 0;
        tess_sound_reset(&s_sound);
        for (int play = 0; play < PLAYS; play++) {
            probe_rest_long(&s_sound);
            bool sounded = tess_sound_cue(&s_sound, (tess_cue_t)cue, k_cue_strength[cue], 0);
            assert(sounded);
            check_on_scale(&s_sound, k_cue_names[cue]);
            probe_t p = {s_storage, 0, 0};
            memset(s_storage, 0, sizeof s_storage);
            probe_finish(&s_sound, &p);
            mean += check_clean(&p, k_cue_db[cue]) / PLAYS;
            check_palette(&p, k_cue_names[cue]);
            check_onset(&p, &k_cue_rules[cue], k_cue_names[cue]);
            signatures[play] = probe_signature(&p);
            for (int before = 0; before < play; before++) assert(signatures[before] != signatures[play]);
        }
        fprintf(stderr, "  %-14s %6.1f dB (class %5.1f)\n", k_cue_names[cue], mean, k_cue_db[cue]);
    }
}

// Layered pads can add coherently. Keep the observed overlap regression and a bounded
// independent sweep, so deleting another event cannot hide this loudness boundary.
static void check_love_variants(void) {
    double quietest = 0, loudest = -100;
    for (unsigned n = 0; n <= 512; n++) {
        unsigned seed = n ? n * 2654435761u : 1795935918u;
        tess_sound_reset(&s_sound);
        tess_sound_seed(&s_sound, seed);
        probe_rest_long(&s_sound);
        probe_t p = probe_cue(&s_sound, s_storage, TC_LOVE, 1, 0);
        double db = check_clean(&p, ALERT);
        if (db < quietest) quietest = db;
        if (db > loudest) loudest = db;
    }
    fprintf(stderr, "love: 513 deterministic variants %.3f..%.3f dB\n", quietest, loudest);
}

// The volume, detent and glint notches: every one in its class, the pitch of the first note climbing with the level.
static void check_notches(void) {
    static const struct { sfx_t event; int last; } k_groups[] = {{SFX_VOLUME, 4}, {SFX_DETENT, 10}, {SFX_GLINT, 10}};
    // Independent seeds make this test insensitive to removed or reordered event fixtures.
    // Sweep the random level/timbre variation rather than selecting one convenient sound.
    for (unsigned seed = 1; seed <= 64; seed++) {
        for (unsigned g = 0; g < sizeof k_groups / sizeof k_groups[0]; g++) {
            uint32_t previous = 0, first = 0;
            for (int index = 0; index <= k_groups[g].last; index++) {
                tess_sound_reset(&s_sound);
                tess_sound_seed(&s_sound, seed * 2654435761u);
                probe_rest(&s_sound);
                tess_sound_event(&s_sound, k_groups[g].event, index, 0, .5f);
                uint32_t step = s_sound.pending[s_sound.pending_head].step;
                assert(step >= previous - previous / 100);
                previous = step;
                if (!index) first = step;
                probe_t p = {s_storage, 0, 0};
                memset(s_storage, 0, sizeof s_storage);
                probe_finish(&s_sound, &p);
                double db = probe_perceived_100ms(&p);
                if (fabs(db - UI_PERCEIVED) > TOLERANCE)
                    fprintf(stderr, "notch event=%d index=%d seed=%u db=%.3f\n", k_groups[g].event, index, seed, db);
                assert(fabs(db - UI_PERCEIVED) <= TOLERANCE);
                assert(probe_peak(&p) < 16384);
            }
            assert(previous + previous / 50 > first * 2);
        }
    }
}

// The strongest scale note (D3..D7) in the first 100 ms: what the register of a sound moves.
static int dominant_hz(const probe_t *p) {
    static const float k_hz[5] = {146.83f, 164.81f, 185.f, 220.f, 246.94f};
    double best = -1;
    int hz = 0;
    for (int degree = 0; degree < SCALE_DEGREES; degree++) {
        float f = k_hz[degree % 5] * (float)(1 << (degree / 5));
        double power = probe_bin(p->pcm, 2400, f);
        if (power > best) { best = power; hz = (int)f; }
    }
    return hz;
}
// The same seed is the same sound for every event and cue; another seed is another sound.
static void check_seed(void) {
    static tess_sound_t other;
    static int32_t second[PROBE_MAX_SAMPLES];
    for (int kind = 0; kind < SFX_COUNT + TC_COUNT; kind++) {
        bool cue = kind >= SFX_COUNT;
        int id = cue ? kind - SFX_COUNT : kind;
        uint64_t signature[3];
        for (int run = 0; run < 3; run++) {
            memset(&other, 0, sizeof other);
            tess_sound_reset(&other);
            tess_sound_seed(&other, run == 2 ? 8 : 7);
            probe_t p;
            if (cue) p = probe_cue(&other, second, (tess_cue_t)id, .6f, 0);
            else p = probe_event(&other, second, (sfx_t)id, notch_of((sfx_t)id), 0);
            signature[run] = probe_signature(&p);
        }
        assert(signature[0] == signature[1]);
        assert(signature[0] != signature[2]);
    }
}

// A tap is never quite the same twice, and it moves by scale steps.
static void check_variation(void) {
    tess_sound_reset(&s_sound);
    tess_sound_seed(&s_sound, 37);
    uint64_t signatures[40];
    int pitches[8] = {0}, kinds = 0;
    for (int play = 0; play < 40; play++) {
        probe_rest(&s_sound);
        probe_t p = probe_event(&s_sound, s_storage, SFX_TAP, -1, 0);
        signatures[play] = probe_signature(&p);
        for (int before = 0; before < play; before++) assert(signatures[before] != signatures[play]);
        int hz = dominant_hz(&p), known = 0;
        for (int i = 0; i < kinds; i++) known |= pitches[i] == hz;
        if (!known && kinds < 8) pitches[kinds++] = hz;
    }
    fprintf(stderr, "tap: %d different pitches in 40 plays\n", kinds);
    assert(kinds >= 3);
}

// What drives what: the tap answers the side of the touch and sits where the speaker is strong; the two record
// chirps differ and both sit where the speaker plays.
static void check_character(void) {
    static int32_t second[PROBE_MAX_SAMPLES];
    long left = 0, right = 0;
    for (int play = 0; play < 12; play++) {
        tess_sound_reset(&s_sound);
        tess_sound_seed(&s_sound, 50 + play);
        probe_rest(&s_sound);
        probe_t p = probe_event(&s_sound, s_storage, SFX_TAP, -1, 0);
        int centre = dominant_hz(&p);
        assert(centre >= 200 && centre <= 450);
        probe_rest(&s_sound);
        p = probe_event(&s_sound, second, SFX_TAP, -1, -.9f);
        left += dominant_hz(&p);
        probe_rest(&s_sound);
        p = probe_event(&s_sound, s_storage, SFX_TAP, -1, .9f);
        right += dominant_hz(&p);
    }
    fprintf(stderr, "tap: left %ld Hz, right %ld Hz on average\n", left / 12, right / 12);
    assert(right > left);
    tess_sound_reset(&s_sound);
    probe_t start = probe_event(&s_sound, s_storage, SFX_LISTEN_START, -1, 0);
    double start_centroid = probe_centroid(&start);
    tess_sound_reset(&s_sound);
    probe_t stop = probe_event(&s_sound, second, SFX_LISTEN_STOP, -1, 0);
    assert(probe_signature(&start) != probe_signature(&stop));
    assert(start_centroid > 300 && probe_centroid(&stop) > 200 && probe_centroid(&stop) < start_centroid);  // opening rises, closing falls, both low
}

void check_rules(void);
// The firmware's first sound may come before anything reset the synth: a sound must never be silent for that (the sine
// table is built on demand; it once was built only by tess_sound_reset and Tess was mute on the device).
static void check_first_sound(void) {
    static tess_sound_t fresh;  // zeroed, never reset
    static int32_t out[2400];
    tess_sound_event(&fresh, SFX_TAP, -1, 0, .5f);
    int32_t peak = 0;
    while (tess_sound_mix(&fresh, out, 240)) {
        for (int i = 0; i < 240; i++) peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    }
    assert(peak > 200);
}

int main(void) {
    check_first_sound();
    check_events();
    check_cues();
    check_love_variants();
    check_notches();
    check_seed();
    check_variation();
    check_character();
    check_rules();
    fprintf(stderr, "palette: at most %.2f %% of any sound's energy above 2.5 kHz, at least %d partials in each, spectral flatness at most %.4f (noise is about .5)\n",
            100 * s_worst_high, s_fewest_partials, s_worst_flatness);
    fprintf(stderr, "onsets: all sounds start softly within %d ms\n", s_slowest_quick);
    puts("Tess sound family: every sound and cue clean, in its loudness class, on the scale, varied; rules passed");
    return 0;
}
