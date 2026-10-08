#include "face_internal.h"

// ---------------------------------------------------------------- update

static void spring_step(face_t *f, float dt) {
    // Semi-implicit Euler is unstable for the stiffest facial springs at a
    // 100ms display stall. Integrate the full elapsed interval in bounded steps.
    int steps = (int)ceilf(dt * 120.f);
    if (steps < 1) steps = 1;
    float step = dt / steps;
    for (int i = 0; i < FACE_NP; i++) {
        float w = 17.f, z = 0.6f;
        if (i == P_LOOKX || i == P_LOOKY) {
            w = 24.f;
            z = 0.8f;
        } else if (i >= P_CR && i <= P_CB) {
            w = 7.f;
            z = 1.f;
        } else if (i == P_LTL || i == P_LTR) {
            w = 26.f;
            z = 0.85f;
        } else if (i == P_HEART || i == P_SLEEPEYE || i == P_DIM) {
            w = 10.f;
            z = 1.f;
        }
        float x = f->cur.v[i], v = f->vel.v[i];
        for (int n = 0; n < steps; n++) {
            float a = w * w * (f->target.v[i] - x) - 2.f * z * w * v;
            v += a * step;
            x += v * step;
        }
        f->cur.v[i] = x;
        f->vel.v[i] = v;
    }
}

// The face rattles about inside the character's glass. It lags the device (a jolt to the right
// throws it left), a soft spring brings it home, and the glass edge bounces it back once per
// jolt; later hits in the same jolt just stop it.
#define LOOSE_PX_PER_GS 16000.f // design px/s gained per g*s of jolt
#define LOOSE_TILT 150.f        // design px of slide per g of sideways pull (~15 deg: the edge)
#define LOOSE_X 42.f            // travel until the face presses into the glass edge, design px
#define LOOSE_Y 28.f
#define LOOSE_W 5.5f            // spring, rad/s (~0.9 Hz)
#define LOOSE_ZETA 0.6f
#define LOOSE_BOUNCE 0.55f      // restitution of the one bounce
#define SWAY_PX_PER_GS 2600.f   // panel px/s the whole plush gains per g*s of jolt
#define SWAY_MAX 14.f           // panel px
#define SWAY_W 9.f              // rad/s (~1.4 Hz), springy
#define SWAY_ZETA 0.3f

// One bounce per hit: a face that bounced re-arms only once it is back near the middle,
// so a face held against the edge (tilted) just rests there.
static void loose_axis(float *x, float *v, float lim, float *squash, bool *armed) {
    if (fabsf(*x) < lim * 0.5f) *armed = true;
    if (fabsf(*x) <= lim) return;
    *x = copysignf(lim, *x);
    if (*v * *x <= 0) return;  // already heading back
    float hit = fabsf(*v);
    *squash = fmaxf(*squash, fminf(0.16f, hit / 3000.f));
    *v = *armed && hit > 40.f ? -*v * LOOSE_BOUNCE : 0.f;
    *armed = false;
}

static void loose_update(face_t *f, float dt) {
    float dvx = f->jolt_dvx, dvy = f->jolt_dvy;
    f->jolt_dvx = f->jolt_dvy = 0;
    if (!has_body(f) || f->mode == MODE_SETUP) {
        f->loose_x = f->loose_y = f->loose_vx = f->loose_vy = 0;
        f->body_dx = f->body_vx = 0;
        return;
    }
    if (hypotf(dvx, dvy) > 0.002f) {  // below that it is sensor noise
        f->loose_vx -= dvx * LOOSE_PX_PER_GS;
        f->loose_vy -= dvy * LOOSE_PX_PER_GS;
        if (fabsf(dvx) > 0.01f) f->body_vx -= dvx * SWAY_PX_PER_GS;  // only a real knock moves the plush
        if (hypotf(dvx, dvy) > 0.012f) f->loose_arm_x = f->loose_arm_y = true;
    }
    // Where gravity would roll it (a little past the edge: a tilted face leans on the glass).
    float tx = clampf(f->slide_x * LOOSE_TILT, -LOOSE_X * 1.3f, LOOSE_X * 1.3f);
    float ty = clampf(f->slide_y * LOOSE_TILT, -LOOSE_Y * 1.3f, LOOSE_Y * 1.3f);
    float h = dt / 4;
    for (int i = 0; i < 4; i++) {
        f->loose_vx += (-LOOSE_W * LOOSE_W * (f->loose_x - tx) - 2 * LOOSE_ZETA * LOOSE_W * f->loose_vx) * h;
        f->loose_vy += (-LOOSE_W * LOOSE_W * (f->loose_y - ty) - 2 * LOOSE_ZETA * LOOSE_W * f->loose_vy) * h;
        f->loose_x += f->loose_vx * h;
        f->loose_y += f->loose_vy * h;
        loose_axis(&f->loose_x, &f->loose_vx, LOOSE_X, &f->squash_x, &f->loose_arm_x);
        loose_axis(&f->loose_y, &f->loose_vy, LOOSE_Y, &f->squash_y, &f->loose_arm_y);
        f->body_vx += (-SWAY_W * SWAY_W * f->body_dx - 2 * SWAY_ZETA * SWAY_W * f->body_vx) * h;
        f->body_dx = clampf(f->body_dx + f->body_vx * h, -SWAY_MAX, SWAY_MAX);
    }
    float decay = expf(-dt * 9.f);
    f->squash_x *= decay;
    f->squash_y *= decay;
}


static float advance_face_clock(face_t *f, float dt) {
    f->qr_t += dt;
    if (dt > 0.1f) dt = 0.1f;
    f->t += dt;
    f->mode_t += dt;
    f->idle_t += dt;
    return dt;
}

static void decay_positive(float *value, float dt) {
    if (*value > 0) *value -= dt;
}

static void update_mode_timers(face_t *f, float dt) {
    if (f->mode == MODE_BOOT) {
        f->boot_t += dt;
        if (f->boot_t > 1.2f) face_set_mode(f, MODE_IDLE);
    }
    if (f->emotion_left > 0) {
        f->emotion_left -= dt;
        if (f->emotion_left <= 0) {
            f->emotion = EMO_NEUTRAL;
            f->emotion_left = -1;
        }
    }
}

static void update_short_timers(face_t *f, float dt) {
    for (int i = 0; i < 2; i++)
        decay_positive(&f->poke_eye_t[i], dt);
    decay_positive(&f->notify_t, dt);
    if (f->bub_icon) {
        f->bub_t += dt;
        if (f->bub_left > 0 && (f->bub_left -= dt) <= 0) f->bub_icon = BUB_NONE;
    }
    decay_positive(&f->dizzy_t, dt);
    decay_positive(&f->volume_show, dt);
    card_update(f, dt);
    f->menu.k = clampf(f->menu.k + (f->menu.open ? dt : -dt) / 0.5f, 0, 1);
    f->menu.pressed_t += dt;
    if (f->agent.open && f->agent.view == AGENT_VIEW_GUIDE && f->agent.guide_elapsed < 1.f)
        f->agent.guide_elapsed = fminf(1.f, f->agent.guide_elapsed + dt);
}

static void update_corner_indicators(face_t *f, float dt) {
    if (f->cron_due >= 0 && f->cron_due < f->t - 5) f->cron_due = -1;  // due and gone: the server tells what is next
    bool cron_on = (f->cron_running > 0 || f->cron_due >= 0) && !f->menu.open && f->mode != MODE_SETUP;
    bool offline_on = f->offline_icon && face_character(f) == CHARACTER_TESS && !f->menu.open && f->mode != MODE_SETUP;
    f->offline_k = clampf(f->offline_k + (offline_on ? dt : -dt) / 0.3f, 0, 1);
    if (offline_on) cron_on = false;  // the corner is the connection's now (cron news is stale anyway)
    f->cron_k = clampf(f->cron_k + (cron_on ? dt : -dt) / 0.3f, 0, 1);
}

static void update_blink(face_t *f, float dt) {
    // Blinking: 2.5-6 s apart, sometimes double. Suppressed while asleep.
    if (f->mode != MODE_SLEEP && f->mode != MODE_BOOT) {
        f->next_blink -= dt;
        if (f->blink_phase == 0 && f->next_blink <= 0) {
            f->blink_phase = 1;
            f->blink_t = 0;
        }
    }
    if (f->blink_phase) {
        f->blink_t += dt;
        const float close = 0.07f, hold = 0.03f, open = 0.1f;
        float b;
        if (f->blink_t < close) b = f->blink_t / close;
        else if (f->blink_t < close + hold) b = 1;
        else b = 1 - (f->blink_t - close - hold) / open;
        f->blink = clampf(b, 0, 1);
        if (f->blink_t > close + hold + open) {
            f->blink = 0;
            f->blink_phase = 0;
            bool twice = frand(f) < 0.18f;
            f->next_blink = twice ? 0.12f : frange(f, 2.5f, 6.f) * (f->mode == MODE_LISTENING ? 1.6f : 1.f);
        }
    }
}

static void update_saccade(face_t *f, float dt) {
    // Saccades: small gaze shifts, calmer while listening/speaking.
    f->next_saccade -= dt;
    if (f->next_saccade <= 0) {
        float rx = 24, ry = 14;
        if (f->mode == MODE_LISTENING || f->mode == MODE_SPEAKING) {
            rx = 7;
            ry = 5;
        }
        if (f->mode == MODE_THINKING) {
            f->look_tx = frange(f, -10, 16);
            f->look_ty = frange(f, -10, 2);
        } else if (frand(f) < 0.3f) {
            f->look_tx = f->look_ty = 0;
        } else {
            f->look_tx = frange(f, -rx, rx);
            f->look_ty = frange(f, -ry, ry);
        }
        f->next_saccade = frange(f, 1.2f, 3.6f);
    }
}

static void update_micro_action(face_t *f, float dt) {
    // Idle micro-actions.
    if (f->mode == MODE_IDLE && f->emotion == EMO_NEUTRAL) {
        if (f->micro_left > 0) {
            f->micro_left -= dt;
        } else {
            f->next_micro -= dt;
            if (f->next_micro <= 0) {
                int n = f->idle_t > 45 ? 4 : 3;
                f->micro = (int)(frand(f) * n);
                if (f->idle_t > 45 && frand(f) < 0.5f) f->micro = 3;
                f->micro_left = f->micro == 0 ? 2.2f : 1.6f;
                f->next_micro = frange(f, 10, 22);
            }
        }
    }
}

static void update_speech_mouth(face_t *f, float dt, face_params_t *target) {
    if (f->mode == MODE_SPEAKING) {
        // Mouth follows what is actually audible; small hops on syllable onsets.
        float lvl = clampf(f->spk_level * 1.6f, 0, 1);
        f->talk_open += (lvl - f->talk_open) * clampf(dt * 22, 0, 1);
        if (lvl - f->talk_prev > 0.28f) f->hop_v -= 40 * lvl;
        f->talk_prev = lvl;
        target->v[P_MOPEN] = fmaxf(target->v[P_MOPEN], f->talk_open);
    } else {
        f->talk_open = 0;
    }
}

static void update_microphone(face_t *f, float dt) {
    if (f->mode == MODE_LISTENING) {
        memmove(&f->mic_hist[1], &f->mic_hist[0], sizeof(float) * 7);
        f->mic_hist[0] = clampf(f->mic_level, 0, 1);
    }
    float lv = f->mode == MODE_LISTENING ? clampf(f->mic_level * 1.3f, 0, 1) : 0.f;
    f->listen_lvl += (lv - f->listen_lvl) * (1 - expf(-dt * (lv > f->listen_lvl ? 20.f : 4.f)));
}

static void update_pickup(face_t *f, float dt) {
    if (f->sent_t >= 0 && (f->sent_t += dt) > 0.8f) f->sent_t = -1;
    if (f->pickup_t >= 0) {
        float was = f->pickup_t;
        f->pickup_t += dt;
        if (f->mode == MODE_LISTENING || f->mode == MODE_SPEAKING || f->mode == MODE_THINKING) {
            f->pickup_t = -1;  // busy with the user or with OpenClaw: no show over it
        } else if (was < 0.85f && f->pickup_t >= 0.85f) {
            face_set_emotion(f, EMO_LOVE, 2.8f);
            body_request(f, BA_LOVE);
            f->hop_v -= 110;
            for (int i = 0; i < 4; i++) face_spawn(f, 0, frange(f, 140, 340), frange(f, 300, 380));
            f->pickup_t = -1;
        }
    }
}

static void update_hop(face_t *f, float dt) {
    int steps = (int)ceilf(dt * 120.f);
    if (steps < 1) steps = 1;
    float step = dt / steps;
    for (int n = 0; n < steps; n++) {
        float a = -170.f * f->hop - 15.f * f->hop_v;
        f->hop_v += a * step;
        f->hop += f->hop_v * step;
    }
}

static void update_particles(face_t *f, float dt) {
    f->spawn_t -= dt;
    if (f->spawn_t <= 0) {
        f->spawn_t = 0.45f;
        if (f->emotion == EMO_LOVE) face_spawn(f, 0, frange(f, 120, 360), frange(f, 340, 400));
        if (f->emotion == EMO_JOY || f->emotion == EMO_PROUD) face_spawn(f, 1, frange(f, 90, 390), frange(f, 90, 360));
        if (f->mode == MODE_SLEEP && frand(f) < 0.45f) {
            if (has_body(f)) face_spawn(f, 2, f->scr_cx + f->scr_hw * 1.3f, f->scr_cy - f->scr_hh * 1.1f);
            else face_spawn(f, 2, 330, 150);
        }
    }
    for (int i = 0; i < FACE_MAX_PARTICLES; i++) {
        particle_t *p = &f->parts[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt + (p->kind == 2 ? sinf(f->t * 3 + i) * 12 * dt : 0);
        p->y += p->vy * dt;
    }
}

void face_update(face_t *f, float dt) {
    dt = advance_face_clock(f, dt);
    update_mode_timers(f, dt);
    update_short_timers(f, dt);
    update_corner_indicators(f, dt);
    tess_games_update(f, dt);

    // Hidden or unselected characters do no simulation, sprite lookup or mapping. On a dark panel the
    // character holds still and carries on from the same state when it lights up (dt is capped above).
    if (f->menu.k >= 0.45f || f->mode == MODE_SETUP || f->dark) { f->jolt_dvx = f->jolt_dvy = 0; return; }
    if (tess_games_active(f)) { f->rub_in.path = f->rub_in.turns = 0; }
    else if (face_character(f) != CHARACTER_TESS || (f->mode != MODE_OFFLINE && !f->tess_fallen)) face_rub_update(f, dt);
    if (face_character(f) == CHARACTER_TESS) {
        tess_update(f, dt);
        return;
    }
    loose_update(f, dt);

    update_blink(f, dt);
    update_saccade(f, dt);
    update_micro_action(f, dt);

    compute_target(f);
    face_params_t *t = &f->target;
    t->v[P_LOOKX] += f->look_tx + f->tilt_x * 26;
    t->v[P_LOOKY] += f->look_ty + f->tilt_y * 18;
    update_speech_mouth(f, dt, t);
    update_microphone(f, dt);
    update_pickup(f, dt);
    spring_step(f, dt);

    update_hop(f, dt);
    if (has_body(f)) body_update(f, dt);
    update_particles(f, dt);
}
