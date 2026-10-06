#include "face_internal.h"

// Rubbing takes effort (the meter is rub.c): Plush notices, giggles and squirms, blushes with squinting eyes, and
// wiggles; only a few seconds of vigorous rubbing on the belly make him love it. Each stage has three variants, one
// picked at random for a while and never the one it just did. One swipe gets a blink and a small hop (face.c).
#define BELLY_X0 150.f  // panel px: the belly of the plush body, below the screen
#define BELLY_X1 330.f
#define BELLY_Y0 290.f
#define BELLY_Y1 450.f
enum { NOTICE_GLANCE, NOTICE_TILT, NOTICE_HOP };
enum { GIGGLE_BOUNCE, GIGGLE_LOPSIDED, GIGGLE_SHIMMY };
enum { BLUSH_SQUINT, BLUSH_AWAY, BLUSH_LAUGH };
enum { WIGGLE_SIDES, WIGGLE_BOUNCE, WIGGLE_BOTH };

static const float k_move_s[RUB_STAGES][2] = {{0, 0}, {1.5f, 2.3f}, {1.4f, 2.2f}, {1.5f, 2.4f}, {1.f, 1.6f}};
static const float k_beat_hz[RUB_STAGES] = {0, 0, 3.f, 1.8f, 4.5f};  // giggle hops, blush bobs, wiggle hops

// How ticklish the spot under the finger is, 0..1: the belly, a little on the screen face, elsewhere not at all.
static float finger_area(const face_t *f) {
    float x = f->rub_in.x, y = f->rub_in.y;
    if (face_character(f) == CHARACTER_TESS) return hypotf(x - 240, y - 255) < 220 ? 1.f : 0.f;  // on the cloud
    if (!has_body(f)) return hypotf(x - 240, y - 250) < 190 ? 1.f : .2f;
    if (x > BELLY_X0 && x < BELLY_X1 && y > BELLY_Y0 && y < BELLY_Y1) return 1.f;
    return fabsf(x - f->scr_cx) < f->scr_hw && fabsf(y - f->scr_cy) < f->scr_hh ? .35f : 0.f;
}

static void start_move(face_t *f) {
    const rub_t *r = &f->rub;
    if (r->stage == RUB_NOTICE && r->move == NOTICE_GLANCE) {  // at the finger
        f->look_tx = clampf((f->rub_in.x - CX) * .12f, -26, 26);
        f->look_ty = clampf((f->rub_in.y - 240) * .08f, -18, 18);
        f->next_saccade = 2.f;
    }
    if (r->stage == RUB_NOTICE && r->move == NOTICE_HOP) f->hop_v -= 55;
    if (r->stage == RUB_BLUSH && r->move == BLUSH_AWAY) f->next_saccade = 2.f;
}

// Which moves of a stage hop the plush, as a bit mask over the move numbers. He never slides sideways: the whole body
// shifting redraws most of the panel every frame, more than the link can carry at 30 fps; the face tilts instead.
#define MOVE(m) (1 << (m))
static const uint8_t k_hop[RUB_STAGES] = {0, 0, MOVE(GIGGLE_BOUNCE) | MOVE(GIGGLE_LOPSIDED), 7, MOVE(WIGGLE_BOUNCE) | MOVE(WIGGLE_BOTH)};

// The rhythm of the reaction: hops of the whole plush, on the beat of the stage.
static void beat(face_t *f, float dt) {
    const rub_t *r = &f->rub;
    float hz = k_beat_hz[r->stage];
    if (hz <= 0 || (int)((f->t - dt) * hz) == (int)(f->t * hz)) return;
    if (k_hop[r->stage] & MOVE(r->move)) f->hop_v -= r->stage == RUB_WIGGLE ? 55.f : 40.f;
}

static void reward(face_t *f) {
    face_set_emotion(f, EMO_LOVE, 3.2f);
    f->rub_love = true;
}

void face_rub_update(face_t *f, float dt) {
    rub_t *r = &f->rub;
    rub_input_t *in = &f->rub_in;
    bool ready = f->mode == MODE_IDLE && f->menu.k <= 0;
    if (!ready) {
        r->energy = r->speed = r->turns = r->hot = 0;
        r->stage = r->peak = 0;
    } else {
        rub_update(r, in, dt, in->down ? finger_area(f) : 0.f);
        if (in->path > 0) f->idle_t = 0;
    }
    in->path = 0;
    in->turns = 0;
    if (f->emotion != EMO_LOVE) f->rub_love = false;
    if (face_character(f) != CHARACTER_PLUSH || !ready) return;
    if (r->joy) reward(f);
    if (r->stage && f->emotion == EMO_NEUTRAL) {
        if (rub_move_due(r, dt)) {
            rub_pick_move(r, frand(f));
            r->move_left = frange(f, k_move_s[r->stage][0], k_move_s[r->stage][1]);
            start_move(f);
        }
        beat(f, dt);
    }
}

static void notice_params(const rub_t *r, face_params_t *p) {
    p->v[P_EH] = 77;
    if (r->move == NOTICE_TILT) { p->v[P_TILT] = .13f; p->v[P_SCR] = 1.06f; p->v[P_LOOKY] = -6; }
}

static void giggle_params(const rub_t *r, face_params_t *p, float side) {
    p->v[P_SMILE] = .55f; p->v[P_MCURVE] = .85f; p->v[P_MW] = 26; p->v[P_CHEEK] = .65f;
    if (r->move == GIGGLE_LOPSIDED) { p->v[P_LTR] = .5f; p->v[P_TILT] = -.1f; }
    if (r->move == GIGGLE_SHIMMY) p->v[P_TILT] = .08f * side;
}

static void blush_params(const rub_t *r, face_params_t *p) {
    p->v[P_SMILE] = .8f; p->v[P_MCURVE] = 1.f; p->v[P_MW] = 22; p->v[P_CHEEK] = 1.f;
    p->v[P_LTL] = p->v[P_LTR] = .55f; p->v[P_LBOT] = .2f; p->v[P_EH] = 62;
    if (r->move == BLUSH_AWAY) { p->v[P_LOOKX] = -14; p->v[P_LOOKY] = 12; }
    if (r->move == BLUSH_LAUGH) p->v[P_MOPEN] = .45f;
}

static void wiggle_params(const rub_t *r, face_params_t *p, float side) {
    p->v[P_SMILE] = 1.f; p->v[P_MCURVE] = 1.f; p->v[P_MW] = 30; p->v[P_MOPEN] = .7f; p->v[P_CHEEK] = 1.f;
    p->v[P_LTL] = p->v[P_LTR] = .82f; p->v[P_LBOT] = .25f; p->v[P_EH] = 60;
    if (r->move != WIGGLE_BOUNCE) p->v[P_TILT] = .1f * side;
}

// What the stage does to the face, over the emotion (which stays neutral until the reward).
void face_rub_params(const face_t *f, face_params_t *p) {
    const rub_t *r = &f->rub;
    if (!r->stage || f->mode != MODE_IDLE || f->emotion != EMO_NEUTRAL) return;
    float side = (int)(f->t * 4.5f) & 1 ? 1.f : -1.f;
    switch (r->stage) {
    case RUB_NOTICE: notice_params(r, p); break;
    case RUB_GIGGLE: giggle_params(r, p, side); break;
    case RUB_BLUSH: blush_params(r, p); break;
    default: wiggle_params(r, p, side); break;
    }
}
