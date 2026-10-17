#include "tess_sky_play.h"

// A finger on the glass takes hold of the nearest tool that moves (a string by its middle or by an end), or of an aimed
// source (and turns it), or, finding none, draws the stream towards it. A quick tap on a relay star turns it to its next
// way. A tool brought near where the solution has it clicks into place there, and stays until pulled well away; an aimed
// source clicks the same way. Nothing is ever lost: things stay where they are put.
#define END_GRAB_R 40.f
#define SOURCE_GRAB_R 50.f
#define RELAY_TAP_R 44.f
#define TAP_S .4f
#define TAP_MOVE 22.f
#define SNAP_R 20.f
#define SNAP_COS .978f       // 12 degrees
#define UNSNAP_R 34.f
#define UNSNAP_COS .906f     // 25 degrees
#define AIM_SNAP_COS .985f   // 10 degrees
#define EDGE 30.f
#define HALF_MIN 30.f
#define HALF_MAX 92.f

static float clampp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static bool has_ends(const sky_tool_t *t) { return t->kind == TOOL_STRING || t->kind == TOOL_SPLIT; }

// How near a finger is to the tool, and which part: -1 the middle, 0 or 1 an end.
static float reach_of(const sky_tool_t *t, float x, float y, int *end) {
    float best = hypotf(x - t->x, y - t->y);
    *end = -1;
    if (!has_ends(t)) return best;
    for (int e = 0; e < 2; e++) {
        float s = e ? 1.f : -1.f, d = hypotf(x - (t->x + s * t->dx), y - (t->y + s * t->dy));
        if (d < END_GRAB_R && d < best) { best = d - 20.f; *end = e; }   // (an end is easier to catch than the middle)
    }
    return best;
}
static void land(tess_sky_t *k, float x, float y) {
    const sky_world_t *w = &k->w;
    float best = SKY_GRAB_R;
    k->grab = k->tap_star = -1;
    k->aiming = false;
    k->press_t = 0; k->press[0] = x; k->press[1] = y;
    for (int i = 0; i < w->count_tools; i++) {
        const sky_tool_t *t = &w->tool[i];
        int end = -1;
        float d = t->movable ? reach_of(t, x, y, &end) : 1e9f;
        if (d >= best) continue;
        best = d; k->grab = (int8_t)i; k->grab_end = (int8_t)end;
        k->grab_dx = t->x - x; k->grab_dy = t->y - y;
    }
    for (int i = 0; i < w->count_stars; i++)
        if (w->star[i].relay && hypotf(w->star[i].x - x, w->star[i].y - y) < fminf(best, RELAY_TAP_R)) { best = hypotf(w->star[i].x - x, w->star[i].y - y); k->tap_star = (int8_t)i; k->grab = -1; }
    if (w->source.aim && hypotf(w->source.x - x, w->source.y - y) < fminf(best, SOURCE_GRAB_R)) { k->aiming = true; k->grab = k->tap_star = -1; }
    if (k->grab >= 0 || k->aiming || k->tap_star >= 0) k->hint_tool = -1;
}
// Where a tool is against where the solution has it: how far, and how nearly it lies the same way (a line either way).
static float off_by(const sky_tool_t *t, float *cosine) {
    float a = hypotf(t->dx, t->dy), b = hypotf(t->sdx, t->sdy);
    *cosine = has_ends(t) && a > 1.f && b > 1.f ? fabsf(t->dx * t->sdx + t->dy * t->sdy) / (a * b) : 1.f;
    return hypotf(t->x - t->sx, t->y - t->sy);
}
static void snap(tess_sky_t *k, sky_tool_t *t) {
    float cosine, d = off_by(t, &cosine);
    if (t->snapped && d < UNSNAP_R && cosine > UNSNAP_COS) { t->x = t->sx; t->y = t->sy; t->dx = t->sdx; t->dy = t->sdy; return; }
    t->snapped = d < SNAP_R && cosine > SNAP_COS;
    if (!t->snapped) return;
    t->x = t->sx; t->y = t->sy; t->dx = t->sdx; t->dy = t->sdy;
    t->shown = 1.f;
    k->snaps++;
}
static void drag(tess_sky_t *k, float x, float y) {
    sky_tool_t *t = &k->w.tool[k->grab];
    if (k->grab_end < 0) {
        t->x = clampp(x + k->grab_dx, EDGE, SKY_W - EDGE);
        t->y = clampp(y + k->grab_dy, EDGE + 16.f, SKY_W - EDGE);
        snap(k, t);
        return;
    }
    float s = k->grab_end ? 1.f : -1.f;   // the end under the finger moves, the other stays
    float ox = t->x - s * t->dx, oy = t->y - s * t->dy;   // the other end
    float ex = clampp(x, EDGE, SKY_W - EDGE), ey = clampp(y, EDGE, SKY_W - EDGE);
    float hx = (ex - ox) * .5f * s, hy = (ey - oy) * .5f * s, half = hypotf(hx, hy);
    if (half < 1.f) return;
    float scale = clampp(half, HALF_MIN, HALF_MAX) / half;
    t->dx = hx * scale; t->dy = hy * scale;
    t->x = ox + s * t->dx; t->y = oy + s * t->dy;
    snap(k, t);
}
// The source points at the finger; near its solution's way it clicks there.
static void aim(tess_sky_t *k, float x, float y) {
    sky_source_t *s = &k->w.source;
    float dx = x - s->x, dy = y - s->y, len = hypotf(dx, dy);
    if (len < 24.f) return;
    bool was = s->dx == s->sdx && s->dy == s->sdy;
    s->dx = dx / len; s->dy = dy / len;
    if (s->dx * s->sdx + s->dy * s->sdy < AIM_SNAP_COS) return;
    s->dx = s->sdx; s->dy = s->sdy;
    if (!was) k->snaps++;
}
static void lift(tess_sky_t *k) {
    sky_star_t *s = k->tap_star >= 0 ? &k->w.star[k->tap_star] : NULL;
    bool tap = s && k->press_t < TAP_S && hypotf(k->finger[0] - k->press[0], k->finger[1] - k->press[1]) < TAP_MOVE;
    if (tap) {
        s->dir = (uint8_t)((s->dir + 1) % SKY_DIRS);
        k->turns |= (uint8_t)(1u << k->tap_star);
    }
    k->grab = k->tap_star = -1;
    k->aiming = false;
}

bool sky_touch(const face_t *f, tess_sky_t *k, sky_force_t *force, float dt) {
    const rub_input_t *in = &f->rub_in;
    if (in->down && !k->finger_down) land(k, in->x, in->y);
    else if (in->down && k->grab >= 0) drag(k, in->x, in->y);
    else if (in->down && k->aiming) aim(k, in->x, in->y);
    if (in->down) { k->press_t += dt; k->finger[0] = in->x; k->finger[1] = in->y; }
    else if (k->finger_down) lift(k);
    k->finger_down = in->down;
    if (in->down && k->grab < 0 && !k->aiming && k->tap_star < 0) {
        force->finger = true;
        force->fx = in->x; force->fy = in->y;
    }
    return in->down;
}
