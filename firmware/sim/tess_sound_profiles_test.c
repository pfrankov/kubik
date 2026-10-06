// Verify arrangements by observable queued voices, including register edges.
#include "tess_sound_probe.h"
#include <stdio.h>
#include "../main/tess_sound_rules.h"
static tess_sound_t s_word;
static void profile_boundaries(int style, int expected) {
    const tess_voice_t *lead = NULL, *next = NULL, *layers[3];
    int count = 0;
    for (int i = 0; i < s_word.pending_count; i++) {
        const tess_voice_t *v = &s_word.pending[i];
        if (v->harmony) {
            assert(count < 3);
            for (int j = 0; j < count; j++) assert(v->step != layers[j]->step);
            layers[count++] = v;
            assert(probe_voice_end(v) <= s_word.gate.layer_until);
        } else if (!lead) lead = v;
        else if (!next) next = v;
    }
    assert(count == expected && lead && next);
    if (style == H_AIRY) assert(layers[0]->start > AUDIO_RATE / 20);
    if (style == H_WARM || style == H_SOMBER)
        assert(probe_voice_end(lead) > next->start + AUDIO_RATE / 40);
}
static void status_direction(int seed) {
    const sfx_t events[] = {SFX_HELLO, SFX_CONNECT, SFX_DISCONNECT};
    for (unsigned i = 0; i < sizeof events / sizeof events[0]; i++) {
        memset(&s_word, 0, sizeof s_word); tess_sound_reset(&s_word); tess_sound_seed(&s_word, seed);
        tess_sound_event(&s_word, events[i], -1, 0, .5f);
        uint32_t first = s_word.pending[0].step, last = first;
        for (int j = 0; j < s_word.pending_count; j++)
            if (!s_word.pending[j].harmony) last = s_word.pending[j].step;
        assert(events[i] == SFX_DISCONNECT ? last < first : last > first);
    }
}
static uint32_t voice_vowel(const tess_voice_t *v) {
    return (uint32_t)(((uint64_t)v->mod_step * 256 + v->step / 2) / v->step);
}
static void syllable_palette(const rule_t *rule) {
    int syllables = 0;
    for (int i = 0; i < s_word.pending_count; i++) {
        const tess_voice_t *v = &s_word.pending[i];
        if (v->harmony || !v->sustain) continue;
        uint32_t ratio = voice_vowel(v);
        assert(ratio == 256 || ratio == 512 || ratio == 768);
        syllables++;
    }
    assert(syllables >= rule->notes_min && syllables <= rule->notes_max);
}
static bool short_control(int event) {
    return event == SFX_MENU_OPEN || event == SFX_MENU_CLOSE || event == SFX_PAGE || event == SFX_VOLUME;
}
static bool rich_action(int event) {
    return event == SFX_BOOT || event == SFX_SETUP_OK || event == SFX_POWER_OFF;
}
static void action_palette(void) {
    for (int event = 0; event < SFX_COUNT; event++) {
        memset(&s_word, 0, sizeof s_word); tess_sound_reset(&s_word);
        tess_sound_seed(&s_word, 83);
        tess_sound_event(&s_word, (sfx_t)event, -1, 0, .5f);
        const rule_t *rule = &k_event_rules[event];
        const tess_voice_t *v = &s_word.pending[0];
        syllable_palette(rule);
        assert(s_word.pending_count <= 7);
        if (rule->flags & CAPTURE) {
            assert(!v->harmony && (1LL << 30) / v->attack_step <= AUDIO_RATE * 60 / 1000);
            assert(s_word.pending_count == 2);
            assert(voice_vowel(v) == 256 && voice_vowel(&s_word.pending[1]) == 768);
        } else assert(v->sustain > 0);
        if (short_control(event))
            assert(!rule->harmony && probe_voice_end(v) < AUDIO_RATE / 2);
        if (rich_action(event))
            assert(rule->harmony && s_word.pending_count >= 5);
    }
    for (int cue = 0; cue < TC_COUNT; cue++) {
        memset(&s_word, 0, sizeof s_word); tess_sound_reset(&s_word);
        assert(tess_sound_cue(&s_word, (tess_cue_t)cue, .5f, 0));
        if (cue == TC_IMPACT) {
            assert(s_word.pending_count == 1 && !s_word.pending[0].sustain);
            assert(s_word.pending[0].gain <= 350);
        } else syllable_palette(&k_cue_rules[cue]);
    }

}
static void weighting_reference(void) {
    int32_t pcm[4800];
    probe_t p = {pcm, 4800, 1};
    double levels[2];
    for (int tone = 0; tone < 2; tone++) {
        double hz = tone ? 1000 : 250;
        for (int i = 0; i < 4800; i++) pcm[i] = (int32_t)(1000 * sin(2 * 3.141592653589793 * hz * i / AUDIO_RATE));
        levels[tone] = probe_weighted_100ms(&p, false);
    }
    assert(levels[1] - levels[0] > 7 && levels[1] - levels[0] < 10);
    assert(fabs(levels[1] - probe_loudest_100ms(&p)) < .2);
}
static void contact_pitch_palette(void) {
    // Fixed impact speed gives stable tones; harder impacts lift the same point.
    uint32_t tones[112];
    unsigned distinct = 0;
    uint32_t lowest = UINT32_MAX, highest = 0;
    for (int point = 0; point < 112; point++) {
        float identity = point * (2.f / 111) - 1;
        tess_sound_reset(&s_word);
        assert(tess_sound_cue(&s_word, TC_IMPACT, .65f, identity));
        uint32_t tone = s_word.pending[0].step;
        double hz = tone * ((double)AUDIO_RATE / 4294967296.0);
        static const double scale[] = {739.98885, 880., 987.76660, 1174.65907, 1318.51023,
            1479.97769, 1760., 1975.53321, 2349.31814, 2637.02046};
        bool in_scale = false;
        for (unsigned note = 0; note < sizeof scale / sizeof scale[0]; note++)
            in_scale |= fabs(hz - scale[note]) < .001;
        assert(in_scale); // exact B minor pentatonic: no random detune
        bool seen = false;
        for (int previous = 0; previous < point; previous++) seen |= tones[previous] == tone;
        distinct += !seen; tones[point] = tone;
        tess_sound_reset(&s_word);
        assert(tess_sound_cue(&s_word, TC_IMPACT, .25f, identity));
        assert(s_word.pending[0].step < tone);
        tess_sound_reset(&s_word);
        assert(tess_sound_cue(&s_word, TC_IMPACT, .65f, identity));
        assert(s_word.pending[0].step == tone);
        tess_sound_reset(&s_word);
        assert(tess_sound_cue(&s_word, TC_IMPACT, .1f, identity));
        if (s_word.pending[0].step < lowest) lowest = s_word.pending[0].step;
        tess_sound_reset(&s_word);
        assert(tess_sound_cue(&s_word, TC_IMPACT, 1, identity));
        if (s_word.pending[0].step > highest) highest = s_word.pending[0].step;
    }
    assert(fabs(lowest * ((double)AUDIO_RATE / 4294967296.0) - 739.98885) < .001);
    assert(fabs(highest * ((double)AUDIO_RATE / 4294967296.0) - 2637.02046) < .001);
    assert(distinct == 5);
    tess_sound_reset(&s_word);
}

// Exact FFT-band energy for these short sparks; the existing Butterworth
// projection intentionally attenuates its transition band and is not a bin sum.
static double spark_energy_above(const probe_t *probe, double cutoff) {
    double spectrum[PROBE_FFT / 2];
    probe_spectrum(probe, 0, spectrum);
    double total = 0, above = 0;
    for (int bin = 1; bin < PROBE_FFT / 2; bin++) {
        double energy = spectrum[bin] * spectrum[bin];
        total += energy;
        if (bin * (double)AUDIO_RATE / PROBE_FFT > cutoff) above += energy;
    }
    return total > 0 ? above / total : 0;
}
static void collision_dynamics(void) {
    double levels[3];
    const float force[] = {.2f, .4f, .8f};
    static int32_t pcm[PROBE_MAX_SAMPLES];
    for (int i = 0; i < 3; i++) {
        tess_sound_reset(&s_word); tess_sound_seed(&s_word, 37);
        probe_t note = probe_cue(&s_word, pcm, TC_IMPACT, force[i], 0);
        levels[i] = probe_perceived_100ms(&note);
    }
    // Harder impacts stay louder despite pitch-dependent speaker weighting.
    for (int i = 1; i < 3; i++) assert(levels[i] - levels[i - 1] > 4 && levels[i] - levels[i - 1] < 10);
}

static void collision_tails_finish(void) {
    tess_sound_reset(&s_word);
    uint32_t started = s_word.gate.clock;
    for (int i = 0; i < TESS_VOICES; i++)
        assert(tess_sound_cue(&s_word, TC_IMPACT, .65f, i * (2.f / (TESS_VOICES - 1)) - 1));
    int32_t frame[PROBE_FRAME] = {0};
    tess_sound_mix(&s_word, frame, PROBE_FRAME);
    assert(s_word.active_voices == TESS_VOICES);
    for (int i = 0; i < TESS_VOICES; i++)
        assert(tess_sound_cue(&s_word, TC_IMPACT, .65f, -1));
    memset(frame, 0, sizeof frame);
    tess_sound_mix(&s_word, frame, PROBE_FRAME);
    for (int i = 0; i < TESS_VOICES; i++) {
        assert(s_word.voices[i].stage && s_word.voices[i].start == started);
        assert(s_word.voices[i].env < (1 << 30)); // no restarted/replaced tail
    }
    probe_rest_long(&s_word);
    assert(!tess_sound_active(&s_word));
}

static void granular_contacts(void) {
    memset(&s_word, 0, sizeof s_word);
    tess_sound_reset(&s_word);
    tess_sound_seed(&s_word, 37);
    static int32_t grain[PROBE_MAX_SAMPLES];
    for (int point = 0; point < 112; point++) {
        probe_rest_long(&s_word);
        probe_t single = probe_cue(&s_word, grain, TC_IMPACT, .65f, point * (2.f / 111) - 1);
        assert(probe_peak(&single) > 150 && probe_peak(&single) < 2400);
        assert(single.length <= AUDIO_RATE * 700 / 1000);
        assert(spark_energy_above(&single, 800) > .9);
        assert(spark_energy_above(&single, 6000) < .06);
        assert(probe_perceived_100ms(&single) > -49 && probe_perceived_100ms(&single) < -30);
    }
    contact_pitch_palette();
    tess_sound_reset(&s_word);
    unsigned overlapping = 0;
    static int32_t storage[300 * PROBE_FRAME];
    memset(storage, 0, sizeof storage);
    probe_t probe = {storage, 300 * PROBE_FRAME, 0};
    for (int frame = 0; frame < 300; frame++) {
        if (frame < 100)
            for (int point = 0; point < TESS_VOICES; point++)
                assert(tess_sound_cue(&s_word, TC_IMPACT, .4f, point * (2.f / (TESS_VOICES - 1)) - 1));
        int32_t *pcm = storage + frame * PROBE_FRAME;
        tess_sound_mix(&s_word, pcm, PROBE_FRAME);
        if (s_word.active_voices > 1) overlapping++;
        assert(s_word.active_voices <= TESS_VOICES);
        for (int i = 0; i < PROBE_FRAME; i++) assert(abs(pcm[i]) < 5000); // speaker headroom at Interface 100%
        if (frame == 135) assert(!tess_sound_active(&s_word));
    }
    printf("celesta dense contacts: %.1f dB perceived\n", probe_perceived_100ms(&probe)); fflush(stdout);
    assert(probe_perceived_100ms(&probe) > -40 && probe_perceived_100ms(&probe) < -24);  // quiet even with overlapping contacts
    assert(probe_worst_click(&probe) < 8); // dense voice replacement must not click
    assert(overlapping > 50);  // many simultaneous tiny contacts
    assert(!tess_sound_active(&s_word));
}

void check_profiles(void) {
    collision_dynamics();
    collision_tails_finish();
    granular_contacts();
    weighting_reference();
    action_palette();
    const tess_cue_t cues[] = {TC_LOVE, TC_JOY, TC_SAD, TC_CONTENT, TC_SCARED, TC_GRUMPY, TC_LONELY};
    for (int seed = 1; seed <= 32; seed++) {
        for (unsigned i = 0; i < sizeof cues / sizeof cues[0]; i++)
        for (int strong = 0; strong <= 1; strong++) {
            memset(&s_word, 0, sizeof s_word); tess_sound_reset(&s_word); tess_sound_seed(&s_word, seed);
            assert(tess_sound_cue(&s_word, cues[i], strong ? .9f : .3f, seed % 2 ? 1 : -1));
            profile_boundaries(k_cue_rules[cues[i]].harmony, 2 + strong);
        }
        status_direction(seed);
    }
}
