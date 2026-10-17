#include "tess_sky_play.h"

// How she asks for Skies. A star of gold comes out of her and wanders slowly round her, trailing a little comet tail,
// while her dots nearest it light up; it sings her motif: up the last sky's chord from its root, four struck notes over
// a soft swell, and their echo. It is the only melody she has outside a sky, so it is hers alone. A tap on the star,
// or lifting the flat cube, answers it; ignored, it fades. She asks only while lying flat (a slight tilt at most) and
// free to play; rocking the flat cube three times calls the star at once (that is not the game itself).
#define INVITE_CHANCE .55f
#define CALL_S 7.f             // how long the star stays
#define CALL_ORBIT 168.f       // px from her centre
#define CALL_SPIN .42f         // rad/s
#define CALL_R 64.f            // a tap this near the star answers it
#define LYING_G .45f           // she is lying down when gravity along the glass is under this (a slight tilt of the flat cube)
#define ROCK_G .15f            // a rocking swing crosses this on either side of level
#define ROCK_GAP_S 1.4f        // swings come at least this close together
#define ROCK_SWINGS 3
#define RAISED_G .7f           // raised from flat: gravity along the glass is over this (an answer, like a touch)

static bool inviting(const tess_games_t *g) { return g->sky_invite_t > 0; }
static bool lying(const face_t *f) { return hypotf(f->grav_x, f->grav_y) < LYING_G; }
static bool free_to_ask(const face_t *f) {
    return tess_games_eligible(f) && !f->tess_games.game && !f->dimmed && f->tess_hold[TR_SCATTER] <= 0 && f->tess_points_ready && lying(f);
}
static void place_call(tess_games_t *g) {
    g->sky_call[0] = SKY_CX + CALL_ORBIT * cosf(g->sky_call[2]);
    g->sky_call[1] = SKY_CY + CALL_ORBIT * .92f * sinf(g->sky_call[2]);
}
// Her motif: root, third, fifth, sixth of the sky's scale an octave up, struck, over a swell on the root; then the last
// two again, softly, as an echo.
static void sing_call(face_t *f) {
    static const uint8_t k_step[4] = {0, 2, 3, 4};
    static const float k_at[4] = {.05f, .21f, .37f, .6f};
    int root = f->tess_games.sky_root % 5;
    tess_tone(f, TT_SKY_CHIME, sky_degree_hz(root), .42f, .5f);
    for (int i = 0; i < 4; i++) tess_tone_later(f, TT_SKY_PLUCK, sky_degree_hz(root + 5 + k_step[i]), .62f - .04f * (float)i, i == 3 ? 0.f : .4f, k_at[i]);
    tess_tone_later(f, TT_SKY_PLUCK, sky_degree_hz(root + 8), .24f, .5f, .86f);
    tess_tone_later(f, TT_SKY_PLUCK, sky_degree_hz(root + 9), .2f, 0.f, 1.f);
}
static void call(face_t *f) {
    tess_games_t *g = &f->tess_games;
    g->sky_call[2] = frand(f) * 6.2831853f;
    place_call(g);
    g->sky_invite_t = CALL_S;
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
    if (!inviting(g) || !tess_games_eligible(f)) return false;
    return hypotf(x - g->sky_call[0], y - g->sky_call[1]) < CALL_R && tess_sky_start(f);
}
// (every frame) lifting answers the star; rocking calls it; it wanders and runs out. True when a sky has started.
bool sky_summon_tick(face_t *f, float dt) {
    tess_games_t *g = &f->tess_games;
    if (inviting(g) && !g->game && hypotf(f->grav_x, f->grav_y) > RAISED_G && tess_sky_start(f)) return true;
    if (free_to_ask(f)) rock(f, dt);
    else g->rock_n[0] = g->rock_n[1] = (uint8_t)(g->rock_side[0] = g->rock_side[1] = 0);
    if (!inviting(g)) return false;
    g->sky_invite_t = tess_games_eligible(f) && !g->game ? fmaxf(0, g->sky_invite_t - dt) : 0;
    g->sky_call[2] += CALL_SPIN * dt;
    place_call(g);
    return false;
}
// How far the star has come and how far it has gone: 0..1, for the picture.
float sky_summon_shown(const face_t *f) {
    float left = f->tess_games.sky_invite_t;
    return left <= 0 ? 0.f : smooth01((CALL_S - left) / .7f) * smooth01(left / 1.2f);
}
