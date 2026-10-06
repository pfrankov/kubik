// Tess's moods as they show (tess_feel.c): a neutral mood is the old Tess to the bit; a change of mood moves the body in
// small steps, never a jump, and repaints the palette a bounded number of times; the moods read apart by colour; thinking
// and the noir are the same whatever the mood; and the moods speak rarely and never over a talk. cc via tools/test-render.py
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/tess_draw.c"  // (the palette: tess_color)

#define DT (1.f / 30)
#define MOOD_CUES_PER_MINUTE 6
#define TREMBLE_FIELD 8  // (tess_style_t)
static float g_range[TESS_STYLE_FIELDS];  // how far each field of the style runs across the moods

static face_t start(void) {
    face_t f;
    face_init(&f);
    face_set_character(&f, CHARACTER_TESS);
    face_set_mode(&f, MODE_IDLE);
    return f;
}

static void run(face_t *f, float seconds) { for (int i = 0; i < seconds * 30; i++) face_update(f, DT); }

// A mood of the machine's own kind of arrival: a cross-fade from what it was (the machine does this on an event).
static void enter(face_t *f, mood_t to) {
    mood_machine_t *m = &f->mood;
    memcpy(m->from, m->weights, sizeof m->from);
    m->prev = m->now;
    m->now = (uint8_t)to;
    m->progress = m->in_s = m->quiet_s = 0;
    m->level = m->peak = m->intensity = 1.f;
    m->stage = 0;
    m->pinned = false;
    if (to == MOOD_SLEEPY) face_set_emotion(f, EMO_SLEEPY, -1);
    else if (f->emotion == EMO_SLEEPY) face_set_emotion(f, EMO_NEUTRAL, 0);
}

static face_t in_mood(mood_t mood) {
    face_t f = start();
    run(&f, 3);
    if (mood == MOOD_SLEEPY) face_set_emotion(&f, EMO_SLEEPY, -1);
    face_force_mood(&f, mood, 1.f);
    f.mood.pinned = false;
    return f;
}

static void measure_style_range(void) {
    float low[TESS_STYLE_FIELDS], high[TESS_STYLE_FIELDS];
    for (int mood = 0; mood < MOOD_COUNT; mood++) {
        face_t f = in_mood((mood_t)mood);
        run(&f, 4);
        const float *row = (const float *)&f.style;
        for (int k = 0; k < TESS_STYLE_FIELDS; k++) {
            low[k] = mood && low[k] < row[k] ? low[k] : row[k];
            high[k] = mood && high[k] > row[k] ? high[k] : row[k];
        }
    }
    for (int k = 0; k < TESS_STYLE_FIELDS; k++) g_range[k] = fmaxf(high[k] - low[k], 1e-3f);
}

// 1. Neutral is exact: the mood machine's CALM and a Tess with no machine do the same, to the bit, for a minute.
static void check_neutral_is_exact(void) {
    face_t off = start(), calm = start();
    face_mood_off(&off, true);
    for (int i = 0; i < 60 * 30; i++) {
        face_update(&off, DT);
        face_update(&calm, DT);
        assert(calm.mood.now == MOOD_CALM);
        assert(!memcmp(off.tess_position, calm.tess_position, sizeof off.tess_position));
        assert(!memcmp(&off.style, &calm.style, sizeof off.style));
    }
    puts("  neutral mood: a minute of frames identical to a Tess without moods");
}

// 2. Every change of mood (56 of them) moves the body in small steps: the style, the pose and each dot.
static void check_transitions(void) {
    float worst_style = 0, worst_pose = 0, worst_dot = 0, worst_repaints = 0;
    for (int from = 0; from < MOOD_COUNT; from++)
        for (int to = 0; to < MOOD_COUNT; to++) {
            if (from == to) continue;
            face_t f = in_mood((mood_t)from);
            run(&f, 4);
            enter(&f, (mood_t)to);
            tess_style_t last_style = f.style;
            float last_pose[TP_COUNT], last_dot[TESS_N][3], last_mix[MOOD_COUNT];
            memcpy(last_pose, f.tess_mood.pose, sizeof last_pose);
            memcpy(last_dot, f.tess_position, sizeof last_dot);
            memcpy(last_mix, f.feel.mix, sizeof last_mix);
            int repaints = 0;
            for (int i = 0; i < 6 * 30; i++) {
                face_update(&f, DT);
                for (int k = 0; k < TESS_STYLE_FIELDS; k++) {
                    float step = fabsf(((const float *)&f.style)[k] - ((const float *)&last_style)[k]);
                    if (k != TREMBLE_FIELD) worst_style = fmaxf(worst_style, step / g_range[k]);  // (a grumpy huff starts at once)
                }
                for (int k = 0; k < TP_COUNT; k++) worst_pose = fmaxf(worst_pose, fabsf(f.tess_mood.pose[k] - last_pose[k]));
                for (int p = 0; p < TESS_N; p++)
                    for (int a = 0; a < 3; a++) worst_dot = fmaxf(worst_dot, fabsf(f.tess_position[p][a] - last_dot[p][a]));
                repaints += memcmp(last_mix, f.feel.mix, sizeof last_mix) != 0;
                last_style = f.style;
                memcpy(last_pose, f.tess_mood.pose, sizeof last_pose);
                memcpy(last_dot, f.tess_position, sizeof last_dot);
                memcpy(last_mix, f.feel.mix, sizeof last_mix);
            }
            worst_repaints = fmaxf(worst_repaints, repaints);
        }
    printf("  56 changes: style step %.3f of its range, pose step %.3f rad, dot step %.3f, palette rebuilt up to %.0f times\n", worst_style, worst_pose, worst_dot, worst_repaints);
    assert(worst_style < .25f && worst_pose < .05f && worst_dot < .2f && worst_repaints <= 2 * 32);
}

// 3. Thinking and the noir are what they were, whatever the mood it was in: the style is the neutral one and the
// palette is not tinted.
static void check_held_modes_are_neutral(void) {
    static const face_mode_t held[] = {MODE_THINKING, MODE_OFFLINE};
    for (int h = 0; h < 2; h++)
        for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) {
            face_t f = in_mood((mood_t)mood);
            face_set_mode(&f, held[h]);
            run(&f, 8);
            face_t base = start();
            face_set_mode(&base, held[h]);
            run(&base, 8);
            for (int k = 0; k < TESS_STYLE_FIELDS; k++) assert(fabsf(((const float *)&f.style)[k] - ((const float *)&base.style)[k]) < 1e-4f);
            for (int m = 1; m < MOOD_COUNT; m++) assert(f.feel.mix[m] == 0);
            for (int i = 0; i < TESS_SHADES; i++) assert(tess_color(&f, (i + .5f) / TESS_SHADES) == tess_color(&base, (i + .5f) / TESS_SHADES));
        }
    static const face_mode_t talking[] = {MODE_LISTENING, MODE_SPEAKING};
    for (int t = 0; t < 2; t++) {  // (a talk tints the colours by half)
        face_t f = in_mood(MOOD_SCARED);
        face_set_mode(&f, talking[t]);
        run(&f, 4);
        assert(f.feel.palette > .4f && f.feel.palette < .6f);
    }
    puts("  thinking and offline: every mood leaves the neutral style and the palette");
}

// 2b. Every mood's style is a whole row of finite numbers and leaves it asking for attention now and then (invite > 0).
static void check_style_rows_are_whole(void) {
    for (int mood = 0; mood < MOOD_COUNT; mood++) {
        face_t f = in_mood((mood_t)mood);
        run(&f, 4);
        const float *row = (const float *)&f.style;
        for (int k = 0; k < TESS_STYLE_FIELDS; k++) assert(isfinite(row[k]));
        assert(f.style.invite >= .3f && f.style.zoom >= .5f && f.style.cam > 2.f);
    }
    puts("  style rows: 19 finite values each, invitations never switched off");
}

// 4. The palette is rebuilt only when a mood's colour weight moves (by 1/32): not at all while a mood stays.
static void check_palette_is_steady(void) {
    for (int mood = 0; mood < MOOD_COUNT; mood++) {
        face_t f = in_mood((mood_t)mood);
        f.mood.pinned = mood != MOOD_CALM;
        run(&f, 4);
        float mix[MOOD_COUNT];
        memcpy(mix, f.feel.mix, sizeof mix);
        for (int i = 0; i < 10 * 30; i++) {
            face_update(&f, DT);
            assert(!memcmp(mix, f.feel.mix, sizeof mix));
        }
    }
    puts("  palette: nothing to rebuild while a mood stays");
}

static void lab(uint32_t rgb, float out[3]) {
    float c[3] = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f};
    for (int i = 0; i < 3; i++) c[i] = c[i] <= .04045f ? c[i] / 12.92f : powf((c[i] + .055f) / 1.055f, 2.4f);
    float v[3] = {(.4124f * c[0] + .3576f * c[1] + .1805f * c[2]) / .95047f, .2126f * c[0] + .7152f * c[1] + .0722f * c[2],
                  (.0193f * c[0] + .1192f * c[1] + .9505f * c[2]) / 1.08883f};
    for (int i = 0; i < 3; i++) v[i] = v[i] > .008856f ? cbrtf(v[i]) : 7.787f * v[i] + 16.f / 116;
    out[0] = 116 * v[1] - 16; out[1] = 500 * (v[0] - v[1]); out[2] = 200 * (v[1] - v[2]);
}

// 5. The moods read apart by colour: the near shade of each is far from every other's (Lab distance, CIE76).
static void check_colours_read_apart(void) {
    float best = 1e9f;
    int best_a = 0, best_b = 0;
    for (int a = 0; a < MOOD_COUNT; a++)
        for (int b = a + 1; b < MOOD_COUNT; b++) {
            float la[3], lb[3];
            for (int side = 0; side < 2; side++) {
                face_t f = start();
                memset(f.feel.mix, 0, sizeof f.feel.mix);
                if (side ? b : a) f.feel.mix[side ? b : a] = 1.f;
                lab(tess_color(&f, .97f), side ? lb : la);
            }
            float e = sqrtf((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2]));
            if (e < best) { best = e; best_a = a; best_b = b; }
        }
    printf("  colours: nearest pair %s / %s, near shades %.1f apart\n", mood_name((mood_t)best_a), mood_name((mood_t)best_b), best);
    assert(best >= 25.f);
}

typedef struct { tess_cue_t cue; float at; } heard_t;

// Takes what the face asked of the speaker this frame; a mood's word must not come in a talk or while it is held.
static int take_cues(face_t *f, float now, heard_t *heard, int *count) {
    tess_cue_t cue;
    float strength, position;
    int moods = 0;
    while (face_take_cue(f, &cue, &strength, &position)) {
        if (cue >= TC_CURIOUS && cue <= TC_SETTLE) {
            assert(f->mode == MODE_IDLE);
            moods++;
        }
        if (heard && *count < 64) heard[(*count)++] = (heard_t){cue, now};
    }
    return moods;
}

// 6. A shake startles it at once; the fright that follows comes after the startle has been heard.
static void check_shake_then_fright(void) {
    face_t f = start();
    run(&f, 8);
    face_event(&f, FEV_SHAKE, 0, 0);
    heard_t heard[64];
    int count = 0;
    for (int i = 0; i < 6 * 30; i++) {
        face_update(&f, DT);
        take_cues(&f, i * DT, heard, &count);
    }
    float startle = -1, fright = -1;
    for (int i = 0; i < count; i++) {
        if (heard[i].cue == TC_STARTLE && startle < 0) startle = heard[i].at;
        if (heard[i].cue == TC_SCARED && fright < 0) fright = heard[i].at;
    }
    printf("  shake: startle at %.2f s, fright at %.2f s\n", startle, fright);
    assert(startle >= 0 && fright >= startle + .35f - 1e-3f && fright < startle + 2.5f);
}

// 7. A strong mood fading on its own says one settling sigh, not one at every step down.
static void check_one_sigh_on_decay(void) {
    for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) {
        if (mood == MOOD_SLEEPY) continue;
        face_t f = in_mood((mood_t)mood);
        f.tess_play.invite_in = 1e9f;  // (nobody is asked for anything: being ignored is another mood)
        heard_t heard[64];
        int count = 0, sighs = 0;
        for (int i = 0; i < 90 * 30; i++) {
            face_update(&f, DT);
            take_cues(&f, i * DT, heard, &count);
        }
        for (int i = 0; i < count; i++) sighs += heard[i].cue == TC_SETTLE;
        assert(sighs <= 1 && f.mood.now == MOOD_CALM);
        if (mood == MOOD_CURIOUS || mood == MOOD_SCARED) assert(sighs == 0);  // (a small mood, or one that went through curiosity, goes quietly)
        else assert(sighs == 1);
    }
    puts("  decay: back to calm within 90 s, at most one settling sigh");
}

// 7b. A word waiting for a quiet moment is dropped when the mood moves on to one that has no word.
// (something else keeps being said, so a mood's word has to wait)
static void hold_speech(face_t *f, float seconds) {
    for (int i = 0; i < seconds * 30; i++) {
        f->feel.cue_age = 0;
        face_update(f, DT);
    }
}

static void check_stale_word_is_dropped(void) {
    face_t f = start();
    run(&f, 8);
    tess_feel_event(&f, ME_TAP, .6f, 0);
    hold_speech(&f, .5f);
    assert(f.mood.now == MOOD_CURIOUS && f.feel.cue_pending && f.feel.cue == TC_CURIOUS);
    tess_feel_event(&f, ME_GLAD, .8f, 0);
    hold_speech(&f, .5f);
    assert(f.mood.now == MOOD_PLAYFUL && f.feel.cue_pending && f.feel.cue == TC_PLAYFUL);
    tess_feel_event(&f, ME_HEART, 1.f, 0);
    hold_speech(&f, .5f);
    assert(f.mood.now == MOOD_LOVED && !f.feel.cue_pending);
    heard_t heard[64];
    int count = 0;
    for (int i = 0; i < 4 * 30; i++) {
        face_update(&f, DT);
        take_cues(&f, i * DT, heard, &count);
    }
    for (int i = 0; i < count; i++) assert(heard[i].cue != TC_CURIOUS && heard[i].cue != TC_PLAYFUL);
    puts("  cues: a word for a mood it has left is not said");
}

// A TAP outranks another mood event from the same frame. The touch payload stays with its cue,
// whichever order the two producers report them in.
static void report_mixed_events(face_t *f, bool reverse) {
    if (reverse) tess_feel_event(f, ME_JOLT, .95f, -.9f);
    tess_feel_event(f, ME_TAP, .2f, .7f);
    tess_feel_event(f, ME_TAP, .1f, -.8f);  // a weaker duplicate cannot replace this event's payload
    if (!reverse) tess_feel_event(f, ME_JOLT, .95f, -.9f);
}

static void check_selected_touch_payload(const face_t *f) {
    assert(f->mood.now == MOOD_CURIOUS && f->mood.cause == ME_TAP);
    assert(fabsf(f->mood.level - .6f) < 1e-5f && f->mood.cause_strength == .2f && f->mood.cause_side == .7f);
    assert(f->feel.cue_pending && f->feel.cue == TC_CURIOUS);
    assert(fabsf(f->feel.cue_strength - .6f) < 1e-5f && f->feel.cue_side == .7f);
    assert(f->feel.events == 0);
    for (int e = 0; e < ME_COUNT; e++) assert(f->feel.event_strength[e] == 0 && f->feel.event_side[e] == 0);
}

static void check_released_touch_cue(face_t *f) {
    tess_feel_event(f, ME_JOLT, 1.f, -.9f);  // a later same-mood refresh must not rewrite the waiting word
    hold_speech(f, .4f);
    assert(f->feel.cue_pending && f->feel.cue_strength == .6f && f->feel.cue_side == .7f);
    f->feel.cue_age = 1.f;
    face_update(f, DT);
    tess_cue_t cue;
    float strength, side;
    assert(face_take_cue(f, &cue, &strength, &side));
    assert(cue == TC_CURIOUS && fabsf(strength - .6f) < 1e-5f && side == .7f);
    assert(!f->feel.cue_pending);
}

static void check_mixed_event_order(bool reverse) {
    face_t f = start();
    run(&f, 8);
    report_mixed_events(&f, reverse);
    hold_speech(&f, .5f);
    check_selected_touch_payload(&f);
    check_released_touch_cue(&f);
}

static void check_mixed_event_payload_and_release(void) {
    for (int reverse = 0; reverse < 2; reverse++) check_mixed_event_order(reverse != 0);
    puts("  mixed events: the selected touch keeps its payload through the delayed cue in either arrival order");
}

// A later eventless decay uses its own neutral cause payload, rather than retaining the side from the mood that faded.
static void check_eventless_decay_has_no_side(void) {
    face_t f = start();
    run(&f, 8);
    f.tess_play.invite_in = 1e9f;
    tess_feel_event(&f, ME_SAY_SAD, .4f, -.9f);
    int settle = 0;
    for (int i = 0; i < 40 * 30; i++) {
        face_update(&f, DT);
        tess_cue_t cue;
        float strength, side;
        while (face_take_cue(&f, &cue, &strength, &side))
            if (cue == TC_SETTLE) {
                settle++;
                assert(side == 0.f && fabsf(strength - .21f) < .02f);
            }
    }
    assert(f.mood.now == MOOD_CALM && f.mood.cause == MOOD_VIA_DECAY && f.mood.cause_side == 0.f && settle == 1);
    puts("  idle decay: a settling cue has no direction from an event that ended earlier");
}

// 8. Ten minutes of poking it at random: the moods speak at most six times a minute, and never outside the idle mode.
static unsigned dice(unsigned *seed) { *seed = *seed * 1664525u + 1013904223u; return *seed >> 8; }

static void poke(face_t *f, unsigned *seed, bool talks) {
    static const face_event_t touch[] = {FEV_TAP, FEV_TAP, FEV_PET, FEV_PET, FEV_PICKUP, FEV_NOT_HEARD, FEV_WAKE, FEV_SHAKE};
    static const emotion_t words[] = {EMO_JOY, EMO_SAD, EMO_ANGRY, EMO_LOVE, EMO_SURPRISED, EMO_SLEEPY, EMO_NEUTRAL};
    static const face_mode_t modes[] = {MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_IDLE, MODE_OFFLINE, MODE_IDLE, MODE_SLEEP, MODE_IDLE};
    switch (dice(seed) % 5) {
    case 0: case 1: face_event(f, touch[dice(seed) % 8], 100 + dice(seed) % 300, 100 + dice(seed) % 300); break;
    case 2: face_event(f, FEV_VOLUME, dice(seed) % 100, 0); break;
    case 3: face_set_emotion(f, words[dice(seed) % 7], 3.f); break;
    default: if (talks) face_set_mode(f, modes[dice(seed) % 8]); break;
    }
}

static void check_storm(bool talks) {
    face_t f = start();
    unsigned seed = 12345;
    int per_minute[10] = {0}, total = 0;
    for (int i = 0; i < 10 * 60 * 30; i++) {
        if (i % 45 == 0 && dice(&seed) % 3) poke(&f, &seed, talks);
        face_update(&f, DT);
        int spoken = take_cues(&f, i * DT, NULL, NULL);
        per_minute[i / (60 * 30)] += spoken;
        total += spoken;
    }
    int worst = 0;
    for (int m = 0; m < 10; m++) worst = per_minute[m] > worst ? per_minute[m] : worst;
    printf("  ten minutes of pokes%s: %d mood words, at most %d in a minute\n", talks ? "" : " (never a talk)", total, worst);
    assert(worst <= MOOD_CUES_PER_MINUTE);
}

static void check_silent_sleep(void) {
    face_t f = start();
    // The automatic tired mood and sleep transition retain their animation but have no entry sound.
    for (int i = 0; i < 180 * 30; ++i) {
        face_update(&f, DT);
        if (f.mood.now == MOOD_SLEEPY) assert(!f.feel.cue_pending);
        tess_cue_t cue; float strength, side;
        while (face_take_cue(&f, &cue, &strength, &side)) {}
    }
    face_set_mode(&f, MODE_SLEEP);
    for (int i = 0; i < 20 * 30; ++i) {
        face_update(&f, DT);
        tess_cue_t cue; float strength, side;
        assert(!face_take_cue(&f, &cue, &strength, &side));
    }
}

int main(void) {
    check_silent_sleep();
    measure_style_range();
    check_neutral_is_exact();
    check_transitions();
    check_held_modes_are_neutral();
    check_style_rows_are_whole();
    check_palette_is_steady();
    check_colours_read_apart();
    check_shake_then_fright();
    check_one_sigh_on_decay();
    check_stale_word_is_dropped();
    check_mixed_event_payload_and_release();
    check_eventless_decay_has_no_side();
    check_storm(true);
    check_storm(false);
    puts("tess feel ok");
    return 0;
}
