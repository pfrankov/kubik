#include "tess_internal.h"

#include <string.h>

static bool fallen(const face_t *f) { return f->mode == MODE_OFFLINE || f->tess_fallen; }

void tess_reset(face_t *f) {
    f->tess_phase = f->tess_energy = f->tess_pulse = f->tess_audio = f->tess_record = 0;
    f->tess_points_ready = false;
    memset(f->tess_position, 0, sizeof f->tess_position);
    memset(f->tess_velocity, 0, sizeof f->tess_velocity);
    memset(f->tess_reaction, 0, sizeof f->tess_reaction);
    memset(f->tess_hold, 0, sizeof f->tess_hold);
    memset(f->tess_mode, 0, sizeof f->tess_mode);
    f->tess_agitation = f->tess_assemble = 0;
    for (int i = 0; i < TESS_N; i++) f->tess_form_of[i] = f->tess_shape_of[i] = (uint8_t)i;
    f->tess_shape = 0;
    for (int i = 0; i < TESS_N; i++) f->tess_scatter_of[i] = (uint8_t)i;
    tess_match_prepare();
    f->tess_xw = f->tess_zw = 0;
    f->tess_fallen = false;
    f->tess_touch_t = 10;
    for (int k = 0; k < TESS_RIPPLES; k++) f->tess_ripple[k].age = 99;
    f->tess_look[0] = f->tess_look[1] = f->tess_look_rate[0] = f->tess_look_rate[1] = f->tess_look_to[0] = f->tess_look_to[1] = 0;
    f->tess_next_look = 2;
    f->tess_rigid = 0;
    f->tess_next_fidget = 5;
    f->tess_last_kick = -1;
    f->tess_taps = 0;
    f->tess_tap_gap = 9;
    f->tess_sleep_t = -1;
    f->tess_wake_t = 99;
    f->tess_drift = 0;
    tess_mood_reset(f);
    tess_play_reset(f);
    tess_feel_reset(f);
}

static float tess_hold_for(tess_reaction_t r) {
    static const float hold[TR_COUNT] = {
        [TR_HEART] = 3.5f, [TR_RISE] = .8f, [TR_SAD] = 2.2f,
        [TR_PUZZLED] = 1.8f, [TR_JOY] = 1.4f, [TR_SURPRISE] = .35f, [TR_ANGRY] = 2.f, [TR_SHY] = 2.f,
    };
    return hold[r];
}

void tess_react(face_t *f, tess_reaction_t r, float seconds) {
    if (r == TR_SCATTER) { tess_scatter_begin(f); return; }  // (a burst has its own clock)
    float h = seconds > 0 ? fminf(seconds, 6.f) : tess_hold_for(r);
    f->tess_hold[r] = fmaxf(f->tess_hold[r], h);
    f->tess_pulse = 1;
}

void tess_emotion(face_t *f, emotion_t e, float seconds) {
    if (fallen(f)) return;
    tess_feel_emotion(f, e);
    switch (e) {
    case EMO_LOVE: tess_react(f, TR_HEART, seconds); tess_cue(f, TC_LOVE, 1.f, 0); break;
    case EMO_JOY:
    case EMO_HAPPY:
    case EMO_PROUD: tess_react(f, TR_JOY, seconds); tess_cue(f, TC_JOY, 1.f, 0); break;
    case EMO_SURPRISED: tess_react(f, TR_SURPRISE, 0); tess_react(f, TR_RISE, 0); break;
    case EMO_SAD: tess_react(f, TR_SAD, seconds); tess_cue(f, TC_SAD, 1.f, 0); break;
    case EMO_ANGRY: tess_react(f, TR_ANGRY, seconds); break;
    case EMO_CONFUSED: tess_react(f, TR_PUZZLED, seconds); break;
    case EMO_DIZZY: tess_react(f, TR_SCATTER, 0); f->tess_agitation = fmaxf(f->tess_agitation, .6f); break;
    case EMO_SHY: tess_react(f, TR_SHY, seconds); break;
    case EMO_WINK: tess_mood_kick(f, KICK_WIGGLE, .8f, 240, 255); break;
    default: break;
    }
}

// Character lives in how the body moves. A kick is a one-off shove of the whole cloud (tess_mood_kick): the
// springs of its pose turn it into a bounce, a wobble, a twirl. Each has a random strength, and a tap never
// gets the same one twice in a row.
void tess_kick(face_t *f, int kind, float s) {
    tess_mood_kick(f, kind, s, f->tess_touch_x, f->tess_touch_y);
    if (kind == KICK_SHIVER) f->tess_agitation = fmaxf(f->tess_agitation, .3f * s);
    f->tess_pulse = fmaxf(f->tess_pulse, .5f * s);
}

static int tess_pick(face_t *f, const int *pool, int n) {
    int i = (int)(frand(f) * n) % n;
    if (pool[i] == f->tess_last_kick) i = (i + 1 + (int)(frand(f) * (n - 1))) % n;  // any of the others
    int k = pool[i];
    f->tess_last_kick = k;
    return k;
}

static void tess_look_at(face_t *f, float x, float y, float hold) {
    f->tess_look_to[0] = clampf((x - 240) / 150, -1, 1);
    f->tess_look_to[1] = clampf((y - 255) / 150, -1, 1);
    f->tess_next_look = hold;
}

// What a plain tap does: the centre plays, the rim startles or snuggles; frightened, every tap is a flinch, and loved, a snuggle.
static int tap_kick(face_t *f, bool rim) {
    static const int centre[] = {KICK_HOP, KICK_WIGGLE, KICK_TWIRL, KICK_NOD};
    static const int edge[] = {KICK_FLINCH, KICK_NUZZLE, KICK_WIGGLE};
    if (f->style.flinch >= 1.5f) return KICK_FLINCH;
    if (f->style.flinch <= .7f) return KICK_NUZZLE;
    return rim ? tess_pick(f, edge, 3) : tess_pick(f, centre, 4);
}

// Taps: the centre gets a play reaction, the rim a startle or a snuggle, and a moment later a small follow-through.
// A quick run of taps builds up: each is a little bigger, the third makes it glad (a golden bouncing), then a
// twirl, and past six the points fly apart and it is dizzy. (The press was answered when the finger landed:
// tess_touch.c.) A tap while it dodges is missed.
static void tess_tap(face_t *f, float x, float y) {
    if (f->tess_play.dodge_t > 0) return;
    f->tess_taps = f->tess_tap_gap < 1.2f ? f->tess_taps + 1 : 1;
    f->tess_tap_gap = 0;
    float s = frange(f, .8f, 1.25f) * (1 + .1f * (f->tess_taps - 1));
    if (f->tess_taps >= 6) {
        tess_react(f, TR_SCATTER, 0);
        f->tess_agitation = fmaxf(f->tess_agitation, .7f);
        f->tess_taps = 0;
        f->emotion = EMO_DIZZY; f->emotion_left = 2.4f;
        tess_cue(f, TC_STARTLE, .8f, 0);
        return;
    }
    if (f->tess_taps >= 4) {
        tess_kick(f, KICK_TWIRL, s * 1.3f);
        tess_after(f, .3f, KICK_HOP, .7f);
        tess_cue(f, TC_EXCITE, .9f, 0);
        f->tess_last_kick = KICK_TWIRL;
        return;
    }
    if (f->tess_taps == 3) {
        tess_react(f, TR_JOY, 1.6f);
        tess_kick(f, KICK_HOP, s);
        tess_cue(f, TC_EXCITE, .6f, 0);
        return;
    }
    int k = tap_kick(f, hypotf(x - 240, y - 255) > 120);
    tess_kick(f, k, k == KICK_FLINCH ? s * f->style.flinch : s);  // (a frightened one flinches harder)
    if (k == KICK_FLINCH) {  // peeks back at it a moment later
        f->tess_look_to[0] = clampf(-(x - 240) / 150, -1, 1); f->tess_look_to[1] = 0; f->tess_next_look = .5f;
        tess_after(f, .9f, KICK_NUZZLE, .8f);
    } else {
        tess_look_at(f, x, y, 1.4f);
        tess_after(f, .35f, KICK_WIGGLE, .3f * s);
    }
    if (k == KICK_NOD) tess_react(f, TR_SHY, 1.2f);
}

// Touch, gestures and system events. The mood they imply is recorded quietly (not replayed as a reaction).
void tess_event(face_t *f, face_event_t ev, float x, float y) {
    if (tess_games_event(f, ev, x, y)) return;
    if (fallen(f)) return;
    f->tess_energy = 1;
    f->tess_pulse = 1;
    emotion_t mood = EMO_HAPPY;
    float secs = 1.2f;
    f->tess_next_fidget = fmaxf(f->tess_next_fidget, 4);
    tess_mood_event(f, ev, x);
    tess_play_event(f, ev);
    tess_feel_touch_event(f, ev);
    switch (ev) {
    case FEV_TAP:
        f->tess_touch_x = clampf(x, 0, 480); f->tess_touch_y = clampf(y, 0, 480); f->tess_touch_t = 0;
        tess_tap(f, x, y);
        if (f->emotion == EMO_DIZZY) return;
        break;
    case FEV_PET:  // one stroke or a held finger: a small acknowledgement; the heart is earned by rubbing (tess_rub.c)
        f->tess_touch_x = clampf(x, 0, 480); f->tess_touch_y = clampf(y, 0, 480); f->tess_touch_t = 0;
        tess_kick(f, KICK_NUZZLE, frange(f, .5f, .8f));
        tess_look_at(f, x, y, 2.f);
        return;  // no happy face for it
    case FEV_SHAKE:
        tess_react(f, TR_SCATTER, 0);
        f->tess_agitation = 1;
        tess_cue(f, TC_STARTLE, 1.f, 0);
        tess_kick(f, KICK_HOP, 1.f);
        tess_after(f, TESS_BURST_S, KICK_WIGGLE, .6f);  // shakes it off, once it is whole again
        mood = EMO_DIZZY; secs = 2.4f;
        break;
    case FEV_PICKUP:  // up in the air: a startled stretch and a look round
        tess_react(f, TR_RISE, 0);
        tess_kick(f, KICK_HOP, .4f);
        f->tess_look_to[0] = frand(f) < .5f ? -.8f : .8f; f->tess_look_to[1] = -.4f; f->tess_next_look = .7f;
        break;
    case FEV_WAKE: tess_react(f, TR_RISE, 0); break;
    case FEV_NOTIFY: tess_kick(f, KICK_WIGGLE, 1.f); tess_kick(f, KICK_HOP, .6f); break;
    case FEV_FAIL: tess_react(f, TR_SAD, 0); tess_after(f, .5f, KICK_SHIVER, .8f); mood = EMO_SAD; secs = 2.f; break;
    case FEV_NOT_HEARD: tess_react(f, TR_PUZZLED, 0); mood = EMO_CONFUSED; secs = 2.f; break;
    default: break;
    }
    f->emotion = mood;
    f->emotion_left = secs;
}

void tess_set_mode(face_t *f, face_mode_t old, face_mode_t m) {
    f->tess_pulse = 1;
    tess_feel_mode(f, old, m);
    if (m == MODE_IDLE) f->tess_mood.run = f->tess_mood.len;  // back from something else: a fresh move
    if (old == MODE_SLEEP && m != MODE_SLEEP) {  // the points gather back, then a small hop: awake
        f->tess_wake_t = 0;
        tess_after(f, 1.5f, KICK_HOP, .8f);
        f->tess_next_fidget = 5;
    }
    if (old != m && m == MODE_LISTENING) tess_kick(f, KICK_NOD, .5f);  // "yes?"
    if (old == MODE_THINKING && m == MODE_SPEAKING) tess_kick(f, KICK_NOD, .25f);  // got it: a gentle acknowledgement, no launch
    f->tess_sleep_t = m == MODE_SLEEP ? (old == MODE_SLEEP ? f->tess_sleep_t : 0) : -1;
}

static void update_life_timers(face_t *f, float dt) {
    if (f->tess_sleep_t >= 0) f->tess_sleep_t += dt;
    bool apart = f->tess_sleep_t >= .3f;
    f->tess_drift += ((apart ? 1.f : 0.f) - f->tess_drift) * fminf(1, dt * (apart ? .5f : 2.6f));
    f->tess_wake_t += dt;
    f->tess_tap_gap += dt;
}

static void update_fidgets(face_t *f, float dt) {
    bool calm = f->tess_hold[TR_HEART] <= 0 && f->tess_hold[TR_SCATTER] <= 0 && f->tess_agitation < .2f &&
                f->tess_play.content_t <= 0 && !f->tess_play.down;  // (not while it is purring, or sighing)
    if ((f->tess_next_fidget -= dt) <= 0) {
        if (f->mode == MODE_IDLE && calm && f->tess_mode[TM_SLEEP] < .3f) {
            static const int pool[] = {KICK_HOP, KICK_WIGGLE, KICK_TWIRL, KICK_NOD, KICK_SHIVER};
            int k = tess_pick(f, pool, 5);
            tess_kick(f, k, frange(f, .35f, .65f));
            f->tess_next_fidget = frange(f, 5.f, 11.f);
        } else if (f->mode == MODE_OFFLINE && calm) {  // restless: it sighs, and stirs the heap now and then
            if (frand(f) < .5f) tess_kick(f, KICK_NOD, frange(f, .2f, .35f));
            else tess_kick(f, KICK_WIGGLE, frange(f, .3f, .5f));
            f->tess_next_fidget = frange(f, 4.f, 9.f);
        } else {
            f->tess_next_fidget = 2;
        }
    }
}

static void choose_gaze(face_t *f) {
    float *to = f->tess_look_to;
    switch (f->mode) {
    case MODE_LISTENING:
    case MODE_SPEAKING:
        to[0] = frange(f, -.15f, .15f); to[1] = 0;
        f->tess_next_look = frange(f, 1.f, 2.5f);
        break;
    case MODE_THINKING:
        to[0] = to[0] > 0 ? -.7f : .7f; to[1] = -.75f;
        f->tess_next_look = frange(f, .8f, 1.6f);
        break;
    case MODE_OFFLINE:
        to[0] = frange(f, -.4f, .4f); to[1] = .6f;
        f->tess_next_look = frange(f, 2.f, 4.f);
        break;
    case MODE_SLEEP:
        to[0] = to[1] = 0;
        f->tess_next_look = 3;
        break;
    default: {
        bool home = f->tess_mood.attentive || frand(f) < .45f;  // attentive: at you
        to[0] = home ? 0 : frange(f, -.85f, .85f);
        to[1] = home ? 0 : frange(f, -.6f, .5f);
        f->tess_next_look = frange(f, 1.2f, 4.f);
        break;
    }
    }
}

static void update_gaze(face_t *f, float dt) {
    // Gaze: at the speaker while listening or talking, up and aside while thinking, around the room when idle.
    if ((f->tess_next_look -= dt) <= 0) {
        choose_gaze(f);
    }
    // The camera turns to it on a critically damped spring: a glance starts and stops gently, never with a jerk.
    for (int k = 0; k < 2; k++) {
        f->tess_look_rate[k] += (36.f * (f->tess_look_to[k] - f->tess_look[k]) - 12.f * f->tess_look_rate[k]) * dt;
        f->tess_look[k] += f->tess_look_rate[k] * dt;
    }
}

static void update_modes(face_t *f, float dt) {
    static const face_mode_t k_mode_of[TM_COUNT] = {MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_SLEEP, MODE_OFFLINE};
    float *mw = f->tess_mode;
    for (int i = 0; i < TM_COUNT; i++) {
        float target = f->mode == k_mode_of[i] ? 1.f : 0.f;
        if (i == TM_SLEEP && f->mode == MODE_IDLE) target = .45f * f->mood.weights[MOOD_SLEEPY] * f->feel.shown;  // drowsy: as sleepy as the mood is
        mw[i] += (target - mw[i]) * fminf(1, dt * (i == TM_LISTEN ? 5.f : 2.6f));
    }
}

// How hard every dot is pushed about at random this frame: shaking, the shiver of anger, and the mood's (fright, a huff).
float tess_tremble(const face_t *f) {
    float shake = fmaxf(f->tess_agitation * 7.f, f->tess_reaction[TR_ANGRY] * (1 - f->tess_reaction[TR_HEART]) * 2.5f);
    return fmaxf(shake, f->style.tremble);
}

static void update_jolts(face_t *f, float dt) {
    // Jolts and shaking build agitation, which makes every dot tremble on its own (a random push each, so they do not
    // move together: a rigid body does not shake), and makes it giddy (tess_mood). The body as a whole only moves in
    // the startle of a hard shake (FEV_SHAKE, below).
    float jolt = hypotf(f->jolt_dvx, f->jolt_dvy);
    f->jolt_dvx = f->jolt_dvy = 0;
    if (jolt > .002f) {  // below that it is sensor noise
        f->tess_agitation = fminf(1, f->tess_agitation + jolt * 7.f);
        tess_gaze_vibration(f, jolt);
    }
    f->tess_agitation *= expf(-dt * 1.3f);
    float push = tess_tremble(f);
    if (push > TESS_TREMBLE_FROM && f->tess_hold[TR_SCATTER] <= 0) {  // (a burst sets every dot's place itself: nothing to shake)
        for (int i = 0; i < TESS_N; i++)
            for (int axis = 0; axis < 3; axis++) f->tess_velocity[i][axis] += (frand(f) - .5f) * push;
    }
}

static void update_4d_turn(face_t *f, float dt) {
    float *mw = f->tess_mode;
    // 4D: a steady double rotation while thinking, slower and voice-led while replying.
    // After the conversation each angle finishes its quarter turn (the tesseract
    // maps onto itself every quarter) and rests, so at rest it only turns about the vertical.
    {
        float think = mw[TM_THINK], speak = mw[TM_SPEAK], *ang[2] = {&f->tess_xw, &f->tess_zw};
        const float rate[2] = {1.25f, .55f};
        for (int k = 0; k < 2; k++) {
            float a = *ang[k];
            if (f->mode == MODE_THINKING || think > .3f || speak > .03f) {
                float waiting = f->mode == MODE_THINKING || think > .3f ? fmaxf(think, .3f) : think;
                // Keep the quiet tail moving until the quarter-turn return takes over.
                a += dt * rate[k] * fmaxf(.3f, waiting + speak * (.22f + .58f * f->tess_audio));
            }
            else {
                float rest = ceilf(a / (PI / 2) - .001f) * (PI / 2);
                a = fminf(rest, a + dt * fmaxf(.3f, (rest - a) * 1.5f) * rate[k]);
            }
            if (a > 2 * PI) a -= 2 * PI;
            *ang[k] = a;
        }
    }
}

static void update_activity(face_t *f, float dt) {
    float *mw = f->tess_mode;
    // The endless turn is tess_mood's spin (either way, .22 rad/s in every state but idle and the talking ones);
    // the lively boosts follow its direction, and vanish as it rests.
    float spin = f->tess_mood.spin;
    float speed = spin * (1 - .68f * mw[TM_SLEEP] - .45f * mw[TM_OFFLINE] - .27f * f->tess_drift) + .5f * mw[TM_THINK];  // asleep: nearly still
    f->tess_phase += dt * (speed + clampf(spin / .1f, -1, 1) * (f->tess_energy * .3f + f->tess_agitation * 1.4f) + f->tess_mood.twirl_now);
    if (f->tess_phase > 20 * PI) f->tess_phase -= 20 * PI;
    if (f->tess_phase < -20 * PI) f->tess_phase += 20 * PI;
    float level = f->mode == MODE_SPEAKING ? f->spk_level : f->mode == MODE_LISTENING ? f->mic_level : 0;
    f->tess_audio += (clampf(level * 1.4f, 0, 1) - f->tess_audio) * fminf(1, dt * (level > f->tess_audio ? 20 : 6));
    f->tess_energy += (fmaxf(f->tess_audio, f->tess_agitation) - f->tess_energy) * fminf(1, dt * 5);
    f->tess_record += ((f->mode == MODE_LISTENING ? 1.f : 0.f) - f->tess_record) * fminf(1, dt * 10);
    f->tess_touch_t += dt;
    f->tess_pulse *= fmaxf(0, 1 - dt * 2.8f);
}

static void update_reactions(face_t *f, float dt) {
    tess_scatter_step(f, dt);
    for (int i = 0; i < TR_COUNT; i++) {
        if (i == TR_SCATTER) continue;
        f->tess_hold[i] = fmaxf(0, f->tess_hold[i] - dt);
        float target = f->tess_hold[i] > 0 ? 1 : 0;
        float rate = !target ? 3.f : i == TR_HEART ? 4.f : i == TR_SURPRISE ? 14.f : 7.f;
        f->tess_reaction[i] += (target - f->tess_reaction[i]) * fminf(1, dt * rate);
    }
}

// Only the tesseract is a rigid body; the globe, heart, scatter and drift are other shapes the points pour into.
static bool tess_is_form(const face_t *f) {
    return f->tess_shape == 0 && !f->tess_fallen && (f->live_active || f->tess_mode[TM_LISTEN] < .001f) && f->tess_reaction[TR_HEART] < .001f &&
           f->tess_reaction[TR_SCATTER] < .001f && f->tess_drift < .001f;
}

// (Trembling dots and the waves of a press are the springs' business.)
static bool holds_to_its_places(const face_t *f) {
    if (f->tess_games.ready && (f->tess_games.game || f->tess_games.trick || f->tess_games.fold > 0)) return false;
    return tess_is_form(f) && f->tess_mode[TM_OFFLINE] < .001f && tess_tremble(f) <= TESS_TREMBLE_FROM && !tess_ripple_active(f);
}

// Every point exactly on its target, at rest.
static void sit_on_targets(face_t *f) {
    tess_point_targets(f, f->tess_target, -1);
    memcpy(f->tess_position, f->tess_target, sizeof f->tess_target);
    memset(f->tess_velocity, 0, sizeof f->tess_velocity);
}

// The springs' lag melts away as the cloud settles as the tesseract (about 0.4 s).
static void melt_lag(face_t *f) {
    for (int i = 0; i < TESS_N; i++)
        for (int axis = 0; axis < 3; axis++) {
            float lag = (f->tess_position[i][axis] - f->tess_target[i][axis]) * (1 - f->tess_rigid);
            f->tess_position[i][axis] = f->tess_target[i][axis] + lag;
            f->tess_velocity[i][axis] *= 1 - f->tess_rigid;
        }
}

static void update_points(face_t *f, float dt) {
    if (f->tess_fallen) { tess_fall_step(f, dt); return; }
    int shape = tess_wanted_shape(f);
    if (shape != f->tess_shape && f->tess_points_ready) {
        tess_rematch(f, shape);
        f->tess_shape = (int8_t)shape;
    }

    if (!f->tess_points_ready) {  // a spark at the centre: the springs unfold it
        sit_on_targets(f);
        f->tess_points_ready = true;
        return;
    }
    // Settled as the tesseract, the points sit exactly on their targets: a spring's lag differs from point to point,
    // and a body that only turns must not deform while it does. Any other shape hands them back to the springs.
    // A burst is played on the dots themselves too (tess_scatter.c): no spring may soften it.
    f->tess_rigid = holds_to_its_places(f) ? fminf(1, f->tess_rigid + dt * 2.5f) : 0;
    if (f->tess_rigid >= 1 || f->tess_hold[TR_SCATTER] > 0) {
        sit_on_targets(f);
        return;
    }
    tess_point_targets(f, f->tess_target, f->tess_rigid > 0 ? -1 : f->tess_tick++ & 1);
    tess_springs(f, f->tess_target, dt);
    if (f->tess_rigid > 0) melt_lag(f);
}

void tess_update(face_t *f, float dt) {
    // Offline the points fall; when the connection is back they rise and gather (matched to the nearest places).
    bool fall = f->mode == MODE_OFFLINE && f->tess_points_ready;
    if (fall != f->tess_fallen) {
        tess_fall_turn(f, fall);
        f->tess_shape = -1;
    }

    if (fallen(f)) {
        update_modes(f, dt);
        f->rub_in = (rub_input_t){0};
        f->jolt_dvx = f->jolt_dvy = 0;
        update_points(f, dt);
        return;
    }
    bool playing = tess_games_active(f);
    if (!playing) tess_feel_update(f, dt);
    else tess_touch_waves_step(f, dt);
    update_modes(f, dt);
    f->tess_assemble = fminf(1, f->tess_assemble + dt / 1.2f);
    update_jolts(f, dt);
    update_4d_turn(f, dt);
    if (!playing) {
        tess_mood_update(f, dt);
        tess_rub_step(f, dt);
    }
    update_activity(f, dt);
    // Its own small life: where it looks, fidgets, drifts during sleep, and schedules delayed reactions.
    update_life_timers(f, dt);
    if (!playing) {
        tess_touch_update(f, dt);
        tess_play_update(f, dt);
        update_fidgets(f, dt);
        update_gaze(f, dt);
    }
    update_reactions(f, dt);
    update_points(f, dt);
}
