#include "face_internal.h"

// ---------------------------------------------------------------- body

const char *const k_body_names[BA_COUNT] = {"idle", "wave",  "listen", "think", "talk",  "joy", "love",
                                                   "sad",  "surprised", "sleep", "dizzy", "angry", "shy"};

// Shortest paths through the existing per-frame pose links. A poor direct cut can
// go through another clip's matching pose instead; every edge still plays to its
// own exit frame. Built only when Plush is loaded, never in the frame loop.
static uint8_t body_next[BA_COUNT][BA_COUNT];
void body_graph(const body_t *b) {
    uint32_t cost[BA_COUNT][BA_COUNT];
    for (int a = 0; a < BA_COUNT; a++) for (int to = 0; to < BA_COUNT; to++) {
        const sprite_anim_t *an = sprite_anim(b->id[a]);
        int best = 255;
        for (int k = 0; an && k < an->count; k++) {
            int q; sprite_link(b->id[a], k, b->id[to], &q);
            if (q < best) best = q;
        }
        cost[a][to] = a == to ? 0 : (uint32_t)best * best + 1600;
        body_next[a][to] = to;
    }
    for (int k = 0; k < BA_COUNT; k++) for (int a = 0; a < BA_COUNT; a++) for (int to = 0; to < BA_COUNT; to++) {
        uint32_t via = cost[a][k] + cost[k][to];
        if (via < cost[a][to]) { cost[a][to] = via; body_next[a][to] = body_next[a][k]; }
    }
}

void body_request(face_t *f, body_anim_t a) {
    if (f->character == CHARACTER_PLUSH && f->body.id[a] >= 0) f->body.req = a;
}

// Held animation wanted by the current state (-1 = none).
static int body_want(face_t *f) {
    emotion_t e = f->emotion;
    switch (f->mode) {
    case MODE_LISTENING: return BA_LISTEN;
    case MODE_THINKING: return BA_THINK;
    case MODE_SLEEP: return BA_SLEEP;
    case MODE_SETUP: return BA_THINK;
    case MODE_SPEAKING:
        if (e == EMO_SAD) return BA_SAD;
        if (e == EMO_LOVE) return BA_LOVE;
        if (e == EMO_SHY) return BA_SHY;
        return BA_TALK;
    default: break;
    }
    if (e == EMO_SAD) return BA_SAD;
    if (e == EMO_LOVE) return f->rub_love ? -1 : BA_LOVE;  // the hug clip on top of the reward is more than the link carries
    if (e == EMO_SHY) return BA_SHY;
    return -1;
}


static void body_start(face_t *f, int a, bool oneshot) {
    body_t *b = &f->body;
    b->exit_to = -1;
    b->cur = a;
    b->pos = 0;
    b->vel = 0;
    b->dir = 1;
    b->oneshot = oneshot;
    b->leaving = false;
}

// Carry on from frame `i` of the shown sprite anim into body anim `to` (BA_IDLE = the rest
// pose) through the pose link: the cut lands on the frame that looks most like `i`.
static void body_cut(face_t *f, int from, int i, int to, bool oneshot) {
    body_t *b = &f->body;
    int q, j = sprite_link(from, i, b->id[to], &q);  // always a plain cut: no see-through blends
    if (to == BA_IDLE && !oneshot) {
        // Rest is idle's first frame: land a frame or two in and rewind onto it.
        b->cur = -1;
        b->rest = 0;
        b->pos = 0;
        if (j > 0) {
            body_start(f, BA_IDLE, true);
            b->pos = (float)j;
            b->dir = -1;
        }
        return;
    }
    const sprite_anim_t *an = sprite_anim(b->id[to]);
    body_start(f, to, oneshot);
    b->pos = (float)j;
    b->vel = an ? an->fps : 0.f;  // already moving: no ease-in from a standstill
}

// Frame of the playing clip to leave through towards `to`: the best pose match, traded
// against how far away it is (q units per frame of travel). Playing on through the clip's
// own ending is cheap; running it backwards (a rewound video) is a last resort.
#define BODY_LINK_PER_FRAME 5.f
#define BODY_LINK_BACK 3.f   // backwards travel costs this much more per frame
#define BODY_LINK_TURN 60.f  // plus this for the turn itself
static int body_exit(const body_t *b, const sprite_anim_t *an, int to) {
    int best = (int)lrintf(b->pos), last = an->count - 1;
    float best_s = 1e9f;
    for (int k = 0; k <= last; k++) {
        int q;
        sprite_link(b->id[b->cur], k, b->id[to], &q);
        float d = k - b->pos;
        float s = q + (d >= -0.5f ? BODY_LINK_PER_FRAME * fmaxf(d, 0.f)
                                  : BODY_LINK_TURN + BODY_LINK_PER_FRAME * BODY_LINK_BACK * -d);
        if (s < best_s) {
            best_s = s;
            best = k;
        }
    }
    return best;
}

typedef struct {
    int want, to, last, exit_k;
    bool urgent, fidget, leaving, has_loop, sleeping, hold;
    float pos0;
} body_motion_t;

static int body_wanted(face_t *f) {
    int want = body_want(f);
    return want >= 0 && f->body.id[want] < 0 ? -1 : want;
}

static void body_update_rest(face_t *f, float dt, int want) {
    body_t *b = &f->body;
    b->rest += dt;
    // From the rest pose (idle's first frame) through its pose link.
    if (b->req >= 0) {
        int hop = body_next[BA_IDLE][b->req];
        bool final = hop == b->req;
        body_cut(f, b->id[BA_IDLE], 0, hop, final);
        if (final) b->req = -1;
    } else if (want >= 0) {
        body_cut(f, b->id[BA_IDLE], 0, body_next[BA_IDLE][want], false);
    } else if ((f->mode == MODE_IDLE || f->mode == MODE_OFFLINE) && b->rest > b->next_idle && !f->rub.stage) {  // not while rubbed
        body_cut(f, b->id[BA_IDLE], 0, frand(f) < 0.12f ? BA_WAVE : BA_IDLE, true);
        b->next_idle = frange(f, 3.f, 9.f);
    }
    if (b->cur >= 0) b->rest = 0;
}

static bool body_is_fidget(const body_t *b, int want) {
    return b->cur == BA_IDLE && b->oneshot && b->dir > 0 && (want >= 0 || b->req >= 0);
}

static bool body_is_tail(const body_t *b, int want, int last) {
    if (!b->oneshot || b->dir <= 0 || b->pos < last * 0.75f) return false;
    return !(b->cur == BA_IDLE && want < 0 && b->req < 0);
}

static bool body_should_leave(const body_t *b, int want, bool urgent, bool fidget, bool tail) {
    if (b->oneshot) return urgent || fidget || tail;
    return b->cur != want || b->req >= 0;
}

static int body_goal(const body_t *b, int want) {
    if (b->req >= 0) return b->req;
    return want >= 0 ? want : BA_IDLE;
}

static void body_plan_motion(face_t *f, const sprite_anim_t *an, int want, body_motion_t *m) {
    body_t *b = &f->body;
    m->last = an->count - 1;
    m->has_loop = an->loop_b > an->loop_a;
    // Something else is wanted: head for the frame whose pose leads into it (see
    // body_exit) and cut there. A press of the talk key cuts even one-shots short; an
    // idle fidget gives way to anything.
    m->urgent = f->mode == MODE_LISTENING && b->cur != BA_LISTEN;
    m->fidget = body_is_fidget(b, want);
    // A gesture in its last quarter already looks for the best way out ahead (its very
    // last frame is rarely the best match for what comes next).
    bool tail = body_is_tail(b, want, m->last);
    m->leaving = body_should_leave(b, want, m->urgent, m->fidget, tail);
    m->to = body_next[b->cur][body_goal(b, want)];
    // Keep one exit pose per destination while the speed eases around.
    if (!m->leaving) b->exit_to = -1;
    else if (b->exit_to != m->to) {
        b->exit_to = m->to;
        b->exit_k = body_exit(b, an, m->to);
    }
    m->exit_k = -1;
    if (m->leaving) {
        m->exit_k = b->exit_k;
        b->dir = m->exit_k >= b->pos ? 1 : -1;
    }
    if (!m->leaving && b->leaving) b->dir = 1;  // wanted again: carry on into the hold
    b->leaving = m->leaving;
    m->pos0 = b->pos;
    m->sleeping = !m->leaving && !b->oneshot && m->has_loop && b->cur == BA_SLEEP;
    m->hold = !m->leaving && !b->oneshot && m->has_loop;
}

static void body_turn_loop(body_t *b, const sprite_anim_t *an, float acc) {
    float stop = b->vel * b->vel / (2 * acc);
    if (b->dir > 0) {
        if (b->pos + stop >= an->loop_b) b->dir = -1;
    } else if (b->dir < 0 && b->pos - stop <= an->loop_a) {
        b->dir = 1;
    }
}

static bool body_sleep_reached(const body_t *b, const sprite_anim_t *an, float pos0) {
    return fabsf(b->pos - an->loop_b) < 0.03f ||
           (pos0 - an->loop_b) * (b->pos - an->loop_b) <= 0;
}

static void body_advance_motion(face_t *f, float dt, const sprite_anim_t *an, const body_motion_t *m) {
    body_t *b = &f->body;
    // Advance on elapsed time; raster interpolation fills the source-frame intervals.
    // The speed eases (no jerk when a clip turns round, rewinds or hurries off).
    float speed = 1.f;
    if (m->leaving && (m->urgent || m->fidget || !b->oneshot)) speed = 1.6f;
    float acc = an->fps * 3.5f;  // full speed in ~0.3 s
    if (!m->leaving && !b->oneshot && m->has_loop && !m->sleeping) {
        // The hold swings between loop_a and loop_b, braking into each turn.
        body_turn_loop(b, an, acc);
    }
    float wanted_v = b->dir * speed * an->fps;
    if (m->sleeping) {
        // The source is a bow, not a breathing loop. Ease into its resting pose once.
        float distance = an->loop_b - b->pos;
        wanted_v = copysignf(fminf(an->fps, sqrtf(2 * acc * fabsf(distance))), distance);
    }
    float dv = wanted_v - b->vel;
    b->vel += clampf(dv, -acc * dt, acc * dt);
    b->pos += b->vel * dt;
    if (m->sleeping && body_sleep_reached(b, an, m->pos0)) {
        b->pos = an->loop_b;
        b->vel = 0;
    }
    if (m->hold) b->pos = clampf(b->pos, 0, m->last);  // braking overshoot never ends a held clip
}

static bool body_reached_exit(const body_t *b, const body_motion_t *m) {
    return m->leaving &&
        ((m->pos0 - m->exit_k) * (b->pos - m->exit_k) <= 0 || fabsf(b->pos - m->exit_k) < 0.5f);
}

static bool body_clip_ended(const body_t *b, const body_motion_t *m) {
    return !m->hold && (b->pos >= m->last + 0.999f || b->pos < 0);
}

static void body_finish_motion(face_t *f, const body_motion_t *m, int want) {
    body_t *b = &f->body;
    if (body_reached_exit(b, m)) {
        bool once = m->to == b->req;
        if (once) b->req = -1;
        body_cut(f, b->id[b->cur], m->exit_k, m->to, once);
    } else if (body_clip_ended(b, m)) {
        // A one-shot ran out: on into whatever is wanted, or back to rest.
        int end = b->pos < 0 ? 0 : m->last;
        int next = body_next[b->cur][body_goal(b, want)];
        bool once = next == b->req;
        if (once) b->req = -1;
        if (b->cur == BA_IDLE && next == BA_IDLE) {
            b->cur = -1;
            b->rest = 0;
            b->pos = 0;
        } else {
            body_cut(f, b->id[b->cur], end, next, once);
        }
    }
}

static void body_update_active(face_t *f, float dt, int want) {
    const sprite_anim_t *an = sprite_anim(f->body.id[f->body.cur]);
    body_motion_t motion = {0};
    body_plan_motion(f, an, want, &motion);
    body_advance_motion(f, dt, an, &motion);
    body_finish_motion(f, &motion, want);
}

static void body_update_screen(face_t *f, float dt) {
    body_t *b = &f->body;
    int cur = b->cur >= 0 ? b->cur : BA_IDLE;
    b->anim = b->id[cur];
    b->frame = b->cur >= 0 ? b->pos : 0;
    float breathe = 1.f;
    const sprite_anim_t *shown = sprite_anim(b->anim);
    if (f->mode == MODE_SLEEP && b->cur == BA_SLEEP && shown && fabsf(b->pos - shown->loop_b) < 0.04f)
        breathe += 0.012f * (0.5f - 0.5f * cosf(f->mode_t * 1.2f));
    f->body_breath += (breathe - f->body_breath) * fminf(1.f, dt * 5.f);
    // Face screen follows the frame; the procedural hop moves the whole body.
    f->body_dy = f->hop * 0.7f;
    sprite_screen_t sc;
    bool ok = sprite_screen(b->anim, b->frame, &sc);
    if (ok) {
        float dx = floorf(lrintf(f->body_dx) * 0.5f) * 2, dy = floorf(lrintf(f->body_dy) * 0.5f) * 2;
        f->scr_cx = sc.cx + dx;
        f->scr_cy = sc.cy + dy;
        f->scr_hw = sc.hw;
        f->scr_hh = sc.hh;
        f->scr_ang = sc.ang;
        f->scr_vis = sc.vis;
        f->scr_has_quad = sc.has_quad;
        for (int k = 0; k < 8; k++) f->scr_quad[k] = sc.quad[k] + (k & 1 ? dy : dx);
        f->scr_has_glass = sc.has_glass;
        f->scr_gx = sc.gx + dx;
        f->scr_gy = sc.gy + dy;
        memcpy(f->scr_gr, sc.gr, sizeof f->scr_gr);
        float stretch = f->body_breath;
        if (fabsf(stretch - 1) > 0.00001f) {
        f->scr_cy = 450 + (f->scr_cy - 450) * stretch;
        f->scr_hh *= stretch;
        f->scr_gy = 450 + (f->scr_gy - 450) * stretch;
        for (int k = 1; k < 8; k += 2) f->scr_quad[k] = 450 + (f->scr_quad[k] - 450) * stretch;
        for (int k = 0; k < SPRITE_GLASS_N; k++) {
            float sn = sinf(k * 2 * PI / SPRITE_GLASS_N);
            f->scr_gr[k] *= 1 + (stretch - 1) * sn * sn;
        }
        }
    }
}

void body_update(face_t *f, float dt) {
    body_t *b = &f->body;
    int want = body_wanted(f);
    if (b->cur < 0) body_update_rest(f, dt, want);
    if (b->cur >= 0) body_update_active(f, dt, want);
    body_update_screen(f, dt);
}
