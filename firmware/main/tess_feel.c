#include "tess_internal.h"

#include <string.h>

// Feel. What the mood machine (mood.c) says, turned into how Tess looks and sounds. Every mood is one row of gains on
// what the neutral Tess does (tess_style_t); the style now is the neutral row plus each mood's difference from it, by its
// blend weight and how strongly it was entered, so the cross-fade between two moods moves the camera, the colours and the
// behaviour together and continuously. Only turns and views change (see tess_style_t): a mood never reshapes the cloud.
// The machine is fed here too: touches and shakes, the agent's words, the state of the link and the battery.
// Entering a mood may speak (a cue), but only when nothing else is being said just then, and never over a talk.
#define STYLE_FADE 2.5f        // 1/s at which the body and the colours give in to (or return from) the gate
#define CUE_QUIET_S .35f       // s after any cue before an entry cue may play
#define CUE_LIFE_S 2.f         // s an entry cue waits for its turn before it is dropped
#define HUFF_LO 2.f            // grumpy: s between two huffs
#define HUFF_HI 4.f
#define HUFF_PUSH .45f
#define HUFF_S .4f
#define SHAKE_DELAY_S .45f

// zoom cam wdist xw pitch sink | dot halo tremble breath pulse | spin rest turn4d attention saccade | flinch dodge invite
static const tess_style_t k_style[MOOD_COUNT] = {
    [MOOD_CALM] = {1.f, 4.7f, 3.4f, 0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f},
    [MOOD_CURIOUS] = {1.06f, 3.9f, 3.f, .22f, -.04f, 0.f, 1.f, 1.f, 0.f, 1.f, 0.f, 1.f, .7f, 1.5f, 1.4f, 1.5f, 1.f, 1.f, 1.f},
    [MOOD_PLAYFUL] = {1.f, 4.7f, 3.4f, 0.f, 0.f, 0.f, 1.05f, 1.15f, 0.f, 1.8f, 0.f, 1.4f, .6f, 2.2f, 1.f, 1.3f, 1.f, 1.75f, 1.6f},
    [MOOD_LOVED] = {1.05f, 5.5f, 3.8f, 0.f, -.06f, 0.f, 1.1f, 1.25f, 0.f, 1.3f, .03f, .6f, 1.f, 1.f, 1.3f, .8f, .5f, .25f, 1.f},
    [MOOD_SCARED] = {.8f, 4.7f, 2.6f, 0.f, .12f, 0.f, .95f, 1.f, .55f, .4f, 0.f, .15f, 1.f, .5f, 1.f, 2.f, 2.f, 1.f, .5f},
    [MOOD_GRUMPY] = {1.f, 4.7f, 3.4f, 0.f, 0.f, 0.f, .95f, .7f, 0.f, .6f, 0.f, 1.f, .8f, .7f, .35f, .6f, 1.6f, 2.5f, .6f},
    [MOOD_SAD] = {.9f, 4.7f, 3.4f, 0.f, .22f, .22f, 1.f, .75f, 0.f, .7f, 0.f, .4f, 1.6f, .5f, .7f, .8f, 1.f, .7f, .6f},
    [MOOD_SLEEPY] = {.95f, 5.5f, 4.2f, 0.f, .12f, .14f, .92f, .6f, 0.f, 1.5f, 0.f, .35f, 2.5f, .5f, .6f, .5f, .5f, .5f, .5f},
};
_Static_assert(sizeof(tess_style_t) == TESS_STYLE_FIELDS * sizeof(float), "the style is a row of floats");

void tess_feel_reset(face_t *f) {
    tess_feel_t *k = &f->feel;
    memset(k, 0, sizeof *k);
    mood_reset(&f->mood);
    f->style = k_style[MOOD_CALM];
    for (int i = 0; i < MOOD_COUNT; i++) k->level[i] = 1.f;
    k->shown = k->palette = 1.f;
    k->huff_in = HUFF_LO;
    k->rng = 2463534242u;
    k->cue_age = 9.f;
}

void tess_feel_event(face_t *f, mood_event_t event, float strength, float side) {
    tess_feel_t *k = &f->feel;
    if ((unsigned)event >= ME_COUNT) return;
    bool already_pending = (k->events & ME_BIT(event)) != 0;
    k->events |= ME_BIT(event);
    if (!already_pending || strength >= k->event_strength[event]) {
        k->event_strength[event] = strength;
        k->event_side[event] = side;
    }
}

// What an emotion set on it means: the agent's tags, the app's welcome, the heart of the rubbing (which comes with rub.joy).
void tess_feel_emotion(face_t *f, emotion_t emotion) {
    switch (emotion) {
    case EMO_LOVE: tess_feel_event(f, f->rub.joy ? ME_HEART : ME_SAY_LOVE, 1.f, 0); break;
    case EMO_JOY:
    case EMO_HAPPY:
    case EMO_PROUD: tess_feel_event(f, ME_SAY_JOY, .6f, 0); break;
    case EMO_SAD: tess_feel_event(f, ME_SAY_SAD, .6f, 0); break;
    case EMO_ANGRY: tess_feel_event(f, ME_SAY_ANGRY, .6f, 0); break;
    case EMO_SURPRISED: tess_feel_event(f, ME_SAY_SURPRISE, .6f, 0); break;
    default: break;
    }
}

// A touch, a gesture or a system event.
void tess_feel_touch_event(face_t *f, face_event_t event) {
    static const uint8_t k_event[] = {
        [FEV_TAP] = ME_TAP, [FEV_PET] = ME_PET, [FEV_SHAKE] = ME_SHAKE, [FEV_PICKUP] = ME_PICKUP, [FEV_NOTIFY] = ME_WAKE,
        [FEV_NOT_HEARD] = ME_FAIL, [FEV_FAIL] = ME_FAIL, [FEV_WAKE] = ME_WAKE, [FEV_TALK_START] = ME_TALK,
    };
    if (event == FEV_VOLUME) return;
    tess_feel_event(f, (mood_event_t)k_event[event], event == FEV_SHAKE ? 1.f : .5f, 0);
}

// A change of mode: the talk turn's answer, the connection lost and back (not the first time it comes up).
void tess_feel_mode(face_t *f, face_mode_t old, face_mode_t mode) {
    tess_feel_t *k = &f->feel;
    if (old == MODE_THINKING && mode == MODE_SPEAKING) tess_feel_event(f, ME_ANSWER, .5f, 0);
    if (mode == MODE_OFFLINE && old != MODE_OFFLINE && k->was_online) { tess_feel_event(f, ME_LINK_LOST, .6f, 0); k->lost = true; }
    if (old == MODE_OFFLINE && mode != MODE_OFFLINE && k->lost) { tess_feel_event(f, ME_LINK_BACK, .6f, 0); k->lost = false; }
    if (mode == MODE_IDLE || mode == MODE_LISTENING || mode == MODE_THINKING || mode == MODE_SPEAKING) k->was_online = true;
    k->cue_pending = false;  // (nothing said before it will be said after)
}

// The entry cue of the mood just entered, if it is to have one. The trigger's own word is played already for a heart, a
// happy or sad word and a shake (its startle); the app has its own chimes for a lost link, a failure, a wake or a turn.
static const uint32_t k_own_word = ME_BIT(ME_HEART) | ME_BIT(ME_SAY_LOVE) | ME_BIT(ME_SAY_JOY) | ME_BIT(ME_SAY_SAD);
static const uint32_t k_app_word = ME_BIT(ME_FAIL) | ME_BIT(ME_LINK_LOST) | ME_BIT(ME_LINK_BACK) | ME_BIT(ME_WAKE) | ME_BIT(ME_TALK) | ME_BIT(ME_ANSWER);
static const uint32_t k_kindness = ME_BIT(ME_PET) | ME_BIT(ME_RUB1) | ME_BIT(ME_RUB3) | ME_BIT(ME_GLAD);

static bool calm_cue(const mood_machine_t *m, tess_cue_t *cue, float *strength) {
    bool by_event = m->cause < ME_COUNT;
    if (!by_event && m->cause == MOOD_VIA_DECAY) {  // it settled by itself, after all its time: a whisper
        *cue = TC_SETTLE;
        *strength = .3f * m->prev_peak;
        return m->prev != MOOD_CURIOUS && m->prev_peak >= .5f;
    }
    *cue = TC_CONTENT;  // soothed: the sigh
    return by_event && (ME_BIT(m->cause) & k_kindness) && (m->prev == MOOD_SCARED || m->prev == MOOD_GRUMPY || m->prev == MOOD_SAD);
}

static bool entry_cue(const mood_machine_t *m, tess_cue_t *cue, float *strength, float *delay) {
    uint32_t cause = m->cause < ME_COUNT ? ME_BIT(m->cause) : 0;
    *strength = m->level;
    *delay = 0;
    if (cause & (k_own_word | k_app_word)) return false;
    switch (m->now) {
    case MOOD_CURIOUS: *cue = TC_CURIOUS; return m->prev == MOOD_CALM;
    case MOOD_PLAYFUL: *cue = TC_PLAYFUL; return cause != 0;
    case MOOD_SCARED: *cue = TC_SCARED; *delay = SHAKE_DELAY_S; return true;
    case MOOD_GRUMPY: *cue = TC_GRUMPY; return cause != 0;
    case MOOD_SAD: *cue = TC_LONELY; return m->cause == ME_IGNORED;
    case MOOD_SLEEPY: return false;
    case MOOD_CALM: return calm_cue(m, cue, strength);
    default: return false;
    }
}

static bool may_speak(const face_t *f) {
    return f->mode == MODE_IDLE && f->emotion != EMO_SLEEPY && !f->dark && !f->tess_fallen;
}

static void plan_cue(face_t *f) {
    tess_feel_t *k = &f->feel;
    tess_cue_t cue;
    float strength, delay;
    k->cue_pending = false;  // (the word of a mood it has left is not said over the new one)
    if (!may_speak(f) || !entry_cue(&f->mood, &cue, &strength, &delay)) return;
    k->cue = (uint8_t)cue;
    k->cue_strength = strength;
    k->cue_side = f->mood.cause_side;
    k->cue_delay = delay;
    k->cue_life = CUE_LIFE_S + delay;
    k->cue_pending = true;
}

// It plays when nothing else has been said for a moment and no finger or rub is going on.
static void release_cue(face_t *f, float dt) {
    tess_feel_t *k = &f->feel;
    if (!k->cue_pending) return;
    k->cue_delay -= dt;
    if ((k->cue_life -= dt) <= 0 || !may_speak(f)) { k->cue_pending = false; return; }
    if (k->cue_delay > 0 || k->cue_age < CUE_QUIET_S || f->tess_play.down || f->rub.stage) return;
    k->cue_pending = false;
    tess_cue(f, (tess_cue_t)k->cue, k->cue_strength, k->cue_side);
}

// Its own dice (a moody Tess must not change what the rest of it does by chance).
static float huff_dice(tess_feel_t *k) {
    k->rng ^= k->rng << 13; k->rng ^= k->rng >> 17; k->rng ^= k->rng << 5;
    return (k->rng & 0xFFFFFF) / (float)0x1000000;
}

// Blends the styles, and the colour weights the drawing reads (in 1/32 steps: the palette is rebuilt only when they change).
static void blend_style(face_t *f, float dt) {
    tess_feel_t *k = &f->feel;
    const mood_machine_t *m = &f->mood;
    bool live = f->mode == MODE_IDLE, bursting = f->tess_hold[TR_SCATTER] > 0, talk = f->mode == MODE_LISTENING || f->mode == MODE_SPEAKING;
    float ease = fminf(1.f, dt * STYLE_FADE);
    k->shown += ((live && !bursting ? 1.f : 0.f) - k->shown) * ease;  // (the burst's choreography is left as it was made)
    k->palette += ((live ? 1.f : talk ? .5f : 0.f) - k->palette) * ease;
    k->level[m->now] = .5f + .5f * m->intensity;
    float *style = (float *)&f->style;
    const float *calm = (const float *)&k_style[MOOD_CALM];
    memcpy(style, calm, sizeof f->style);
    for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) {
        float weight = m->weights[mood] * k->shown * k->level[mood];
        k->mix[mood] = roundf(m->weights[mood] * k->palette * 32.f) / 32.f;
        if (weight <= 0) continue;
        const float *row = (const float *)&k_style[mood];
        for (int i = 0; i < TESS_STYLE_FIELDS; i++) style[i] += weight * (row[i] - calm[i]);
    }
    if (m->weights[MOOD_GRUMPY] > .05f) {  // it huffs now and then: a short shiver
        if ((k->huff_in -= dt) <= 0) { k->huff = 1.f; k->huff_in = HUFF_LO + (HUFF_HI - HUFF_LO) * huff_dice(k); }
        f->style.tremble += m->weights[MOOD_GRUMPY] * k->shown * k->huff * HUFF_PUSH;
    }
    k->huff = fmaxf(0.f, k->huff - dt / HUFF_S);
}

void tess_feel_update(face_t *f, float dt) {
    tess_feel_t *k = &f->feel;
    k->cue_age += dt;
    if (f->rub.rose) tess_feel_event(f, f->rub.stage >= RUB_BLUSH ? ME_RUB3 : ME_RUB1, .5f, 0);
    mood_input_t input = {
        .events = k->events, .drowsy = f->emotion == EMO_SLEEPY, .battery_low = f->battery_low,
        .charging = f->charging, .touching = f->tess_play.down,
        .gate = f->mode == MODE_IDLE ? MOOD_LIVE : f->mode == MODE_LISTENING || f->mode == MODE_SPEAKING ? MOOD_TALK : MOOD_HELD,
    };
    memcpy(input.event_strength, k->event_strength, sizeof input.event_strength);
    memcpy(input.event_side, k->event_side, sizeof input.event_side);
    k->events = 0;
    memset(k->event_strength, 0, sizeof k->event_strength);
    memset(k->event_side, 0, sizeof k->event_side);
    if (!k->off && mood_step(&f->mood, &input, dt)) plan_cue(f);
    blend_style(f, dt);
    release_cue(f, dt);
}

void face_mood_off(face_t *f, bool off) { f->feel.off = off; }

// (tests, the simulator, the device's test hook) It is in this mood now, and stays.
void face_force_mood(face_t *f, mood_t mood, float level) {
    mood_machine_t *m = &f->mood;
    mood_reset(m);
    m->now = (uint8_t)mood;
    memset(m->weights, 0, sizeof m->weights);
    m->weights[mood] = 1.f;
    memcpy(m->from, m->weights, sizeof m->from);
    m->level = m->peak = m->intensity = fminf(1.f, fmaxf(.5f, level));
    m->pinned = mood != MOOD_CALM;
    f->feel.cue_pending = false;
}
