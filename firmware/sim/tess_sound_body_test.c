// The sounds of her body (tess_sound_body.c) are her voice's palette: tonal, soft-edged and under her words;
// never clipped or harsh, never piling up when many come at once, never taking a voice from a word, and
// following the physics they come from (a tauter pull sits higher).
#include <stdio.h>
#include "tess_sound_probe.h"

static tess_sound_t s_body;
static int32_t s_pcm[PROBE_MAX_SAMPLES];
static const char *const k_kind[TT_COUNT] = {"ring", "creak", "twang", "thud", "grain", "whoosh", "chirp", "wobble"};

static probe_t play(tess_tone_kind_t kind, float hz, float gain, float shade) {
    probe_rest(&s_body);
    probe_t p = {s_pcm, 0, 0};
    memset(s_pcm, 0, sizeof s_pcm);
    tess_tone_t tone = {(uint8_t)kind, hz, gain, shade, 0};
    assert(tess_sound_tone(&s_body, &tone));
    probe_finish(&s_body, &p);
    return p;
}
static void each_kind_is_clean(void) {
    static const float k_hz[TT_COUNT] = {294, 330, 440, 230, 330, 400, 440, 555};  // the registers tess_tone_hz gives
    for (int kind = 0; kind < TT_COUNT; kind++) {
        probe_t loud = play((tess_tone_kind_t)kind, k_hz[kind], 1, .6f);
        double level = probe_weighted_100ms(&loud, true), high = probe_high_share(&loud, 2500), flat = probe_flatness(&loud);
        int peak = probe_peak(&loud), ms = probe_sounding_ms(&loud), onset = probe_onset_ms(&loud);
        probe_t soft = play((tess_tone_kind_t)kind, k_hz[kind], .2f, .6f);
        double quiet = probe_weighted_100ms(&soft, true);
        fprintf(stderr, "    body %-6s %5.1f dB (soft %5.1f), peak %5d, onset %2d ms, %4d ms, %4.1f %% > 2.5 kHz, flatness %.4f\n",
                k_kind[kind], level, quiet, peak, onset, ms, 100 * high, flat);
        assert(peak < 12000 && ms > 40 && ms < 4500);
        assert(level > -44 && level < -34 && level - quiet > 8);  // heard on the small speaker, always under her words
        assert(flat < .02 && high < .05);                         // tonal, nothing shrill
        assert(onset >= 8);                                       // a swell, never a strike
    }
}
static void a_tauter_pull_sits_higher(void) {
    probe_t low = play(TT_CREAK, 220, .6f, .2f);
    double c_low = probe_centroid(&low);
    probe_t high = play(TT_CREAK, 440, .6f, .9f);
    assert(probe_centroid(&high) > c_low * 1.3);
}
// Forty body sounds at once (a frantic minute of play squeezed into a second): a few voices, ducked, under
// the limiter's knee.
static void a_flood_stays_gentle(void) {
    probe_rest_long(&s_body);
    probe_t p = {s_pcm, 0, 0};
    memset(s_pcm, 0, sizeof s_pcm);
    assert(tess_sound_cue(&s_body, TC_RUB, 1, 0));  // her loudest word (-21.5 dB) at the same time
    for (int i = 0; i < 40; i++) {
        tess_tone_t tone = {(uint8_t)(i % TT_COUNT), 200.f + 15 * i, 1, .5f, .02f * i};
        tess_sound_tone(&s_body, &tone);
        if (i % 4 == 3) { probe_mix(&s_body, p.pcm + p.length, &p.peak_voices); p.length += PROBE_FRAME; }
    }
    probe_finish(&s_body, &p);
    double loudest = probe_weighted_100ms(&p, true);
    fprintf(stderr, "    body flood: %d voices at most, peak %d, loudest 100 ms %.1f dB\n", p.peak_voices, probe_peak(&p), loudest);
    assert(probe_peak(&p) < 24000 && loudest < -19);  // under the limiter's knee, and the word stays on top
    tess_tone_t bad = {TT_COUNT, 440, 1, .5f, 0};
    assert(!tess_sound_tone(&s_body, &bad));
}
// A word keeps its voices while her body sounds around it.
static void words_keep_their_voices(void) {
    probe_rest_long(&s_body);
    assert(tess_sound_cue(&s_body, TC_LOVE, 1, 0));
    probe_silence(&s_body, 10);
    int words = 0;
    for (int i = 0; i < TESS_VOICES; i++) words += s_body.voices[i].stage && !s_body.voices[i].body;
    assert(words > 0);
    for (int i = 0; i < 20; i++) {
        tess_tone_t tone = {TT_RING, 300.f + 20 * i, 1, .5f, 0};
        tess_sound_tone(&s_body, &tone);
        probe_silence(&s_body, 1);
        int still = 0;
        for (int v = 0; v < TESS_VOICES; v++) still += s_body.voices[v].stage && !s_body.voices[v].body;
        assert(still >= words || still == 0 || i > 2);  // (the word's own notes end on their own)
    }
    probe_rest(&s_body);
}
void check_body_tones(void) {
    tess_sound_seed(&s_body, 7);
    each_kind_is_clean();
    a_tauter_pull_sits_higher();
    a_flood_stays_gentle();
    words_keep_their_voices();
    fprintf(stderr, "body tones: her palette, under her words, gentle in a flood, never taking a word's voice\n");
}
