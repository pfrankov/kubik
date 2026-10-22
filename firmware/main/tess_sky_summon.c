#include "tess_sky_play.h"

// A quiet invitation lives on one of Tess's own vertices. It never leaves her body.
#define INVITE_CHANCE .55f
#define CALL_S 7.f             // how long the star stays
#define CALL_R 64.f            // a tap this near the star answers it
#define LYING_G .45f           // she is lying down when gravity along the glass is under this (a slight tilt of the flat cube)
#define ROCK_G .15f            // a rocking swing crosses this on either side of level
#define ROCK_GAP_S 1.4f        // swings come at least this close together
#define ROCK_SWINGS 3
#define RAISED_G .7f           // raised from flat: gravity along the glass is over this (an answer, like a touch)
#define CORNER_PX 100          // generous corner squares on the 480 px glass
#define HOLD_S 1.5f

static bool inviting(const tess_games_t *g) { return g->sky_invite_t > 0; }
static bool lying(const face_t *f) { return hypotf(f->grav_x, f->grav_y) < LYING_G; }
static bool available(const face_t *f) { return tess_games_eligible(f) && !f->tess_games.game && !f->dimmed; }
static bool free_to_ask(const face_t *f) {
    return available(f) && f->tess_hold[TR_SCATTER] <= 0 && f->tess_points_ready && lying(f);
}
static int corner(const touch_point_t *p) {
    if (!p->down || p->x < 0 || p->x >= 480 || p->y < 0 || p->y >= 480) return -1;
    int x = p->x < CORNER_PX ? 0 : p->x >= 480 - CORNER_PX ? 1 : -1;
    int y = p->y < CORNER_PX ? 0 : p->y >= 480 - CORNER_PX ? 1 : -1;
    return x < 0 || y < 0 ? -1 : x + 2 * y;
}
static uint32_t diagonal_contacts(const touch_frame_t *in) {
    const touch_point_t *a = &in->point[0], *b = &in->point[1];
    int ca = corner(a), cb = corner(b);
    if (ca < 0 || cb < 0 || (ca ^ cb) != 3 || a->id == b->id) return 0;
    if (ca > cb) { const touch_point_t *swap = a; a = b; b = swap; ca = cb; }
    return (uint32_t)(ca + 1) << 16 | (uint32_t)a->id << 8 | b->id;
}
static bool hold_ready(const face_t *f) {
    unsigned game = f->tess_games.game;
    return tess_games_eligible(f) && !f->dimmed && f->tess_points_ready &&
        f->tess_hold[TR_SCATTER] <= 0 && (game == TESS_GAME_NONE || game == TESS_GAME_STRETCH);
}
// A corner chord takes over the first finger's stretch. Once interrupted it cannot become a sling,
// or restart on the same contacts: release the glass before trying again.
static bool hold_corners(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    if (!touch_count(&f->touch_in)) {
        g->sky_hold_owned = false; g->sky_hold_t = 0; g->sky_hold_contacts = 0;
        return false;
    }
    if (g->sky_hold_owned && g->sky_hold_t < 0) return true;
    uint32_t contacts = diagonal_contacts(&f->touch_in);
    if (!hold_ready(f) || !contacts) {
        g->sky_hold_t = g->sky_hold_owned ? -1 : 0;
        return g->sky_hold_owned;
    }
    if (!g->sky_hold_owned) {
        tess_games_begin(f, TESS_GAME_NONE);
        g->sky_hold_owned = g->finger_owned = true;
        g->pressed = false;
    }
    if (contacts != g->sky_hold_contacts) g->sky_hold_t = 0;
    g->sky_hold_contacts = contacts;
    g->sky_hold_t += dt;
    if (g->sky_hold_t >= HOLD_S) {
        tess_sky_start(f);
        g->sky_hold_t = -1; // allocation failure also requires a fresh gesture, never a per-frame retry
    }
    return true;
}
static void call_origin(face_t *f) {
    tess_games_t *g = &f->tess_games;
    float angle = frand(f) * 6.2831853f;
    float c = cosf(angle), sn = sinf(angle), best = -1e9f;
    g->sky_call_vertex = 0;
    for (int i = 0; i < 16; i++) {
        float x = g->vertex_px[i][0], y = g->vertex_px[i][1];
        if (x < 24 || x > 456 || y < 24 || y > 456) continue;
        float out = (x - SKY_CX) * c + (y - SKY_CY) * sn;
        if (out <= best) continue;
        best = out; g->sky_call_vertex = (uint8_t)i;
    }
}
// One low, slowly blooming interval; no struck notes or bright melodic announcement.
static void sing_call(face_t *f) {
    int root = f->tess_games.sky_root % 5;
    tess_tone(f, TT_SKY_INVITE, sky_degree_hz(root) * .5f, .14f, .5f);
    tess_tone_later(f, TT_SKY_INVITE, sky_degree_hz(root + 3) * .5f, .07f, .5f, .3f);
}
static void call_position(tess_games_t *g) {
    g->sky_call[0] = g->vertex_px[g->sky_call_vertex][0];
    g->sky_call[1] = g->vertex_px[g->sky_call_vertex][1];
}
static void call(face_t *f) {
    tess_games_t *g = &f->tess_games;
    call_origin(f);
    g->sky_invite_t = CALL_S;
    call_position(g);
    sing_call(f);
}
bool tess_sky_invite(face_t *f) {
    if (!free_to_ask(f) || inviting(&f->tess_games) || frand(f) > INVITE_CHANCE) return false;
    call(f);
    return true;
}
// Rocking the flat cube side to side (or back and forth) three times calls the star at once.
static void rock(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    const float pull[2] = {f->grav_x, f->grav_y};
    for (int i = 0; i < 2; i++) {
        g->rock_t[i] += dt;
        if (g->rock_t[i] > ROCK_GAP_S) g->rock_n[i] = (uint8_t)(g->rock_side[i] = 0);
        int side = pull[i] > ROCK_G ? 1 : pull[i] < -ROCK_G ? -1 : 0;
        if (!side || side == g->rock_side[i]) continue;
        g->rock_n[i] = g->rock_side[i] ? g->rock_n[i] + 1 : 1;
        g->rock_side[i] = (int8_t)side; g->rock_t[i] = 0;
        if (g->rock_n[i] >= ROCK_SWINGS) {
            g->rock_n[i] = 0;
            if (!inviting(g)) call(f);   // (a star that is out stays as it is)
        }
    }
}
bool tess_sky_calling(const face_t *f, float *x, float *y) {
    const tess_games_t *g = &f->tess_games;
    *x = g->sky_call[0]; *y = g->sky_call[1];
    return inviting(g);
}
bool tess_sky_tap(face_t *f, float x, float y) {
    tess_games_t *g = &f->tess_games;
    if (!inviting(g) || !available(f)) return false;
    return hypotf(x - g->sky_call[0], y - g->sky_call[1]) < CALL_R && tess_sky_start(f);
}
// (every frame) lifting answers the invitation; rocking calls it; it follows Tess and fades. True when a sky has started.
bool sky_summon_tick(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    if (hold_corners(f, dt)) { g->sky_invite_t = 0; return g->game == TESS_GAME_SKY; }
    if (!available(f)) g->sky_invite_t = 0; // invalidate before a lift or new input can answer it
    if (inviting(g) && hypotf(f->grav_x, f->grav_y) > RAISED_G && tess_sky_start(f)) return true;
    if (free_to_ask(f)) rock(f, dt);
    else g->rock_n[0] = g->rock_n[1] = (uint8_t)(g->rock_side[0] = g->rock_side[1] = 0);
    if (!inviting(g)) return false;
    g->sky_invite_t = fmaxf(0, g->sky_invite_t - dt);
    call_position(g);
    return false;
}
// How far the star has come and how far it has gone: 0..1, for the picture.
float sky_summon_shown(const face_t *f) {
    float left = f->tess_games.sky_invite_t;
    return left <= 0 ? 0.f : smooth01((CALL_S - left) / .7f) * smooth01(left / 1.2f);
}
