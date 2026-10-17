#include "tess_sky_internal.h"

#include <math.h>
#include <string.h>

// The stream. 112 particles on integers (the C6 has no FPU): positions and velocities in 1/8 px, accelerations
// in 1/8 px/s^2, time in Q16 seconds. Per frame the elements are turned into integers once; each particle then
// feels the tilt, the finger, the stars, wells and hungry stars as pulls, bounces off strings (the swept path is
// tested, so a fast one cannot tunnel), takes the colour of a prism and is turned by a splitter. A particle that
// leaves the glass, is eaten or grows old comes back out of the source a moment later: the stream never ends and
// never needs more than her 112 dots.
#define G_PX 300.f       // px/s^2 for a full g of tilt
#define FINGER_A 900.f   // px/s^2 at the finger
#define STAR_A 150.f
#define WELL_A 1500.f
#define HUNGRY_A 460.f
#define HUNGRY_EAT 20.f
#define HOLD_K .3f       // a well slows what is this deep inside it (share of its reach)
#define SETTLE_Q8 720    // 2.8 /s of drag at the centre of a pull
#define FILL_RATE .06f   // fill per second per particle inside a star
#define DRAIN .10f       // fill lost per second
#define RESTITUTION 244  // normal-speed retention /256; grazing motion keeps its tangent
#define MAX_AGE 240      // frames a particle lives (8 s)
#define SPLIT_C 3797     // cos and sin of 22 deg, Q12
#define SPLIT_S 1534
#define PLUCK_GAP .09f
#define RELAY_HEART 17.f // a full relay turns what passes this near its centre
#define RELAY_SPEED 150.f

typedef struct { int32_t x, y, r; uint8_t color; int8_t idx; } circ_t;
typedef struct { int32_t ax, ay, bx, by, nx, ny, xmin, xmax, ymin, ymax; int8_t idx; } seg_t;
typedef struct {
    int32_t dt, gx, gy, fx, fy, finger_r, well_drag;
    int32_t well_spring[SKY_TOOLS];
    bool finger;
    int ns, nsplit, nstr, nwell, nprism, nhungry, nrelay;
    circ_t star[SKY_STARS], well[SKY_TOOLS], prism[SKY_TOOLS], hungry[SKY_TOOLS], relay[SKY_STARS];
    int32_t relay_vx[SKY_STARS], relay_vy[SKY_STARS];
    seg_t str[SKY_TOOLS], split[SKY_TOOLS];
} field_t;

uint32_t sky_rand(sky_world_t *k) {
    uint32_t v = k->rng ? k->rng : 0x9E3779B9u;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return k->rng = v;
}
float sky_randf(sky_world_t *k) { return (sky_rand(k) & 0xFFFFFFu) / (float)0x1000000; }
float sky_range(sky_world_t *k, float low, float high) { return low + (high - low) * sky_randf(k); }

static int32_t q3(float px) { return (int32_t)lrintf(px * SKY_Q3); }
// Distance without a square root: the longer leg plus 3/8 of the shorter, within 7% of the truth.
static int32_t dist_approx(int32_t dx, int32_t dy) {
    int32_t a = dx < 0 ? -dx : dx, b = dy < 0 ? -dy : dy;
    return a > b ? a + ((b * 3) >> 3) : b + ((a * 3) >> 3);
}
static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

bool sky_solved(const sky_world_t *k) {
    if (!k->count_stars) return false;
    for (int i = 0; i < k->count_stars; i++)
        if (k->star[i].fill < SKY_FULL) return false;
    return true;
}

// ---------------------------------------------------------------- the field, once a frame
static void load_segment(seg_t *s, const sky_tool_t *t, int idx) {
    float len = hypotf(t->dx, t->dy);
    if (len < 1.f) len = 1.f;
    s->ax = q3(t->x - t->dx); s->ay = q3(t->y - t->dy);
    s->bx = q3(t->x + t->dx); s->by = q3(t->y + t->dy);
    s->nx = (int32_t)lrintf(-t->dy / len * 4096.f); s->ny = (int32_t)lrintf(t->dx / len * 4096.f);
    s->xmin = s->ax < s->bx ? s->ax : s->bx; s->xmax = s->ax > s->bx ? s->ax : s->bx;
    s->ymin = s->ay < s->by ? s->ay : s->by; s->ymax = s->ay > s->by ? s->ay : s->by;
    s->idx = (int8_t)idx;
}
static void load_tool(field_t *F, const sky_tool_t *t, int idx) {
    circ_t c = {q3(t->x), q3(t->y), q3(t->r), t->color, (int8_t)idx};
    switch (t->kind) {
    case TOOL_STRING: load_segment(&F->str[F->nstr++], t, idx); break;
    case TOOL_SPLIT: load_segment(&F->split[F->nsplit++], t, idx); break;
    case TOOL_WELL:
        F->well_spring[F->nwell] = c.r > 0 ? (int32_t)(2.f * WELL_A * SKY_Q3 * 65536.f / (float)c.r) : 0;
        F->well[F->nwell++] = c; break;
    case TOOL_PRISM: F->prism[F->nprism++] = c; break;
    case TOOL_HUNGRY: F->hungry[F->nhungry++] = c; break;
    default: break;
    }
}
// A relay's way, a unit vector: 0 is to the right, then clockwise (screen y is down) by 45 degrees.
void sky_dir_vector(int dir, float *dx, float *dy) {
    static const float k_dir[SKY_DIRS][2] = {{1, 0}, {.7071f, .7071f}, {0, 1}, {-.7071f, .7071f}, {-1, 0}, {-.7071f, -.7071f}, {0, -1}, {.7071f, -.7071f}};
    *dx = k_dir[dir & (SKY_DIRS - 1)][0]; *dy = k_dir[dir & (SKY_DIRS - 1)][1];
}
static void load_relay(field_t *F, const sky_star_t *s) {
    float dx, dy;
    sky_dir_vector(s->dir, &dx, &dy);
    F->relay[F->nrelay] = (circ_t){q3(s->x), q3(s->y), q3(RELAY_HEART), s->color, 0};
    F->relay_vx[F->nrelay] = q3(dx * RELAY_SPEED); F->relay_vy[F->nrelay++] = q3(dy * RELAY_SPEED);
}
// open_relays: every relay turns the light whether or not it is full (a trace follows the solution's light).
static void load_field(field_t *F, const sky_world_t *k, float dt, const sky_force_t *force, bool open_relays) {
    memset(F, 0, sizeof *F);
    F->dt = (int32_t)(dt * 65536.f);
    F->well_drag = 65536 - F->dt * 75 / 128; // 251/256 at 30 Hz, same drag per elapsed second
    F->gx = q3(force->gx * G_PX); F->gy = q3(force->gy * G_PX);
    F->finger = force->finger;
    F->fx = q3(force->fx); F->fy = q3(force->fy); F->finger_r = q3(SKY_FINGER_R);
    for (int i = 0; i < k->count_stars; i++) {
        const sky_star_t *s = &k->star[i];
        F->star[F->ns++] = (circ_t){q3(s->x), q3(s->y), q3(SKY_ZONE_R), s->color, (int8_t)i};
        if (s->relay && (open_relays || s->fill >= SKY_FULL)) load_relay(F, s);
    }
    for (int i = 0; i < k->count_tools; i++) load_tool(F, &k->tool[i], i);
}

// ---------------------------------------------------------------- forces
static void pull(const sky_particle_t *p, int32_t cx, int32_t cy, int32_t reach, int32_t strength, int32_t *ax, int32_t *ay) {
    int32_t dx = cx - p->x, dy = cy - p->y, d = dist_approx(dx, dy);
    if (d >= reach || d < 8) return;
    int32_t mag = strength * (reach - d) / reach;
    *ax += mag * dx / d; *ay += mag * dy / d;
}
// Slows what is in reach, so that what is drawn in stays a while instead of swinging through.
static void settle(const sky_particle_t *p, int32_t cx, int32_t cy, int32_t reach, int32_t rate_q8, int32_t *ax, int32_t *ay) {
    int32_t d = dist_approx(cx - p->x, cy - p->y);
    if (d >= reach) return;
    int32_t k = rate_q8 * (reach - d) / reach;   // /256 per second
    *ax -= p->vx * k / 256; *ay -= p->vy * k / 256;
}
static void pull_well(const sky_particle_t *p, const circ_t *w, int32_t spring, int32_t *ax, int32_t *ay) {
    int32_t dx = w->x - p->x, dy = w->y - p->y, d = dist_approx(dx, dy);
    if (d >= w->r) return;
    int32_t near = (w->r - d) * 256 / w->r;
    int32_t k = (int32_t)((int64_t)spring * near / 256);
    // A soft radial spring: zero at the centre and rim, no undefined direction or central kick.
    *ax += (int32_t)((int64_t)dx * k / 65536);
    *ay += (int32_t)((int64_t)dy * k / 65536);
}
static void accelerate(const field_t *F, const sky_particle_t *p, int32_t *ax, int32_t *ay) {
    *ax = F->gx; *ay = F->gy;
    if (F->finger) {
        pull(p, F->fx, F->fy, F->finger_r, (int32_t)(FINGER_A * SKY_Q3), ax, ay);
        settle(p, F->fx, F->fy, F->finger_r, SETTLE_Q8, ax, ay);
    }
    for (int i = 0; i < F->nwell; i++) pull_well(p, &F->well[i], F->well_spring[i], ax, ay);
    for (int i = 0; i < F->nhungry; i++) pull(p, F->hungry[i].x, F->hungry[i].y, F->hungry[i].r, (int32_t)(HUNGRY_A * SKY_Q3), ax, ay);
    for (int i = 0; i < F->ns; i++) {
        if (F->star[i].color != p->color) continue;
        pull(p, F->star[i].x, F->star[i].y, q3(SKY_STAR_PULL_R), (int32_t)(STAR_A * SKY_Q3), ax, ay);
    }
}

// ---------------------------------------------------------------- what a particle meets
static int64_t side(const seg_t *s, int32_t x, int32_t y) {
    return (int64_t)(s->bx - s->ax) * (y - s->ay) - (int64_t)(s->by - s->ay) * (x - s->ax);
}
// Whether the step from (x0,y0) to (x1,y1) crosses the segment, and where.
static bool crosses(const seg_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t *cx, int32_t *cy, int *from_positive, int32_t *fraction) {
    // Exact broad phase: disjoint bounding boxes cannot meet, including during a reflected leg.
    if ((x0 < s->xmin && x1 < s->xmin) || (x0 > s->xmax && x1 > s->xmax) ||
        (y0 < s->ymin && y1 < s->ymin) || (y0 > s->ymax && y1 > s->ymax)) return false;
    int64_t d0 = side(s, x0, y0), d1 = side(s, x1, y1);
    if ((d0 > 0) == (d1 > 0) || d0 == d1) return false;
    int64_t span = d0 - d1;
    int32_t px = x0 + (int32_t)((int64_t)(x1 - x0) * d0 / span), py = y0 + (int32_t)((int64_t)(y1 - y0) * d0 / span);
    int64_t along = (int64_t)(px - s->ax) * (s->bx - s->ax) + (int64_t)(py - s->ay) * (s->by - s->ay);
    int64_t length2 = (int64_t)(s->bx - s->ax) * (s->bx - s->ax) + (int64_t)(s->by - s->ay) * (s->by - s->ay);
    if (along < 0 || along > length2) return false;
    *cx = px; *cy = py; *from_positive = d0 > 0;
    *fraction = (int32_t)(d0 * 65536 / span);
    return true;
}
static void split(sky_particle_t *p, int parity) {
    int32_t c = SPLIT_C, sn = parity ? SPLIT_S : -SPLIT_S;
    int32_t vx = (p->vx * c - p->vy * sn) / 4096, vy = (p->vx * sn + p->vy * c) / 4096;
    p->vx = (int16_t)vx; p->vy = (int16_t)vy;
}

typedef struct { int32_t x, y, fraction; int from, index; bool split; } contact_t;
static bool first_contact(const field_t *F, int32_t x0, int32_t y0, int32_t x1, int32_t y1, contact_t *hit) {
    hit->fraction = 65537;
    for (int i = 0; i < F->nstr + F->nsplit; i++) {
        bool splitting = i >= F->nstr;
        int index = splitting ? i - F->nstr : i;
        const seg_t *line = splitting ? &F->split[index] : &F->str[index];
        contact_t c = {.index = index, .split = splitting};
        if (crosses(line, x0, y0, x1, y1, &c.x, &c.y, &c.from, &c.fraction) && c.fraction < hit->fraction) *hit = c;
    }
    return hit->fraction <= 65536;
}
static void bounce(sky_particle_t *p, const seg_t *s, const contact_t *c, sky_hits_t *hits) {
    int sign = c->from ? 1 : -1;
    int32_t nx = s->nx * sign, ny = s->ny * sign;
    int32_t into = (p->vx * nx + p->vy * ny) / 4096;
    if (into < 0) {
        p->vx -= (int16_t)(((into * nx / 4096) * (256 + RESTITUTION)) / 256);
        p->vy -= (int16_t)(((into * ny / 4096) * (256 + RESTITUTION)) / 256);
    }
    p->x = (int16_t)(c->x + nx * 2 / 4096); p->y = (int16_t)(c->y + ny * 2 / 4096);
    float strike = (float)(-into) / (SKY_Q3 * 300.f);
    if (hits && strike > hits->strike[s->idx]) hits->strike[s->idx] = strike > 1.f ? 1.f : strike;
}
static void advance_contacts(const field_t *F, sky_particle_t *p, int parity, int32_t x0, int32_t y0, sky_hits_t *hits) {
    int32_t left = F->dt;
    // Follow the actual piecewise path, including splitters before/after a reflection.
    for (int n = 0; n < 4; n++) {
        contact_t c;
        if (!first_contact(F, x0, y0, p->x, p->y, &c)) return;
        left = (int32_t)((int64_t)left * (65536 - c.fraction) / 65536);
        if (c.split) {
            const seg_t *s = &F->split[c.index];
            split(p, parity);
            int side = p->vx * s->nx + p->vy * s->ny < 0 ? -1 : 1;
            p->x = (int16_t)(c.x + side * s->nx * 2 / 4096);
            p->y = (int16_t)(c.y + side * s->ny * 2 / 4096);
        } else bounce(p, &F->str[c.index], &c, hits);
        if (n == 3) return; // At the budget, stay at this safe contact; never tunnel through an unchecked wall.
        x0 = p->x; y0 = p->y;
        p->x += (int16_t)(p->vx * left / 65536);
        p->y += (int16_t)(p->vy * left / 65536);
    }
}

static void respawn(sky_world_t *k, sky_particle_t *p) {
    const sky_source_t *s = &k->source;
    float sideways = sky_range(k, -1.f, 1.f), along = sky_range(k, 0.f, 6.f), speed = s->speed * sky_range(k, .86f, 1.14f);
    float jitter = sky_range(k, -.075f, .075f) * speed;
    p->x = (int16_t)q3(s->x - s->dy * sideways * 5.f + s->dx * along);
    p->y = (int16_t)q3(s->y + s->dx * sideways * 5.f + s->dy * along);
    p->vx = (int16_t)q3(s->dx * speed - s->dy * jitter);
    p->vy = (int16_t)q3(s->dy * speed + s->dx * jitter);
    p->color = COL_TEAL;
    p->age = 0;
    p->wait = (uint8_t)(sky_rand(k) % 22);
}

static bool eaten(const field_t *F, const sky_particle_t *p) {
    for (int i = 0; i < F->nhungry; i++)
        if (dist_approx(p->x - F->hungry[i].x, p->y - F->hungry[i].y) < q3(HUNGRY_EAT)) return true;
    return false;
}
static void hold_in_wells(const field_t *F, sky_particle_t *p, sky_world_t *k) {
    for (int i = 0; i < F->nwell; i++) {
        const circ_t *w = &F->well[i];
        if (dist_approx(p->x - w->x, p->y - w->y) >= (int32_t)(w->r * HOLD_K)) continue;
        p->vx = (int16_t)(p->vx * F->well_drag / 65536); p->vy = (int16_t)(p->vy * F->well_drag / 65536);  // it lingers and circles
        if (k && k->tool[w->idx].held < 100) k->tool[w->idx].held++;
    }
}
static void touch_tools(const field_t *F, sky_world_t *k, sky_particle_t *p, int parity, int32_t x0, int32_t y0, sky_hits_t *hits) {
    advance_contacts(F, p, parity, x0, y0, hits);
    for (int i = 0; i < F->nprism; i++)
        if (dist_approx(p->x - F->prism[i].x, p->y - F->prism[i].y) < F->prism[i].r) p->color = F->prism[i].color;
    hold_in_wells(F, p, k);
    for (int i = 0; i < F->nrelay; i++)
        if (F->relay[i].color == p->color && dist_approx(p->x - F->relay[i].x, p->y - F->relay[i].y) < F->relay[i].r) {
            p->vx = (int16_t)F->relay_vx[i]; p->vy = (int16_t)F->relay_vy[i];
        }
}
static void count_in_stars(const field_t *F, sky_world_t *k, const sky_particle_t *p) {
    for (int i = 0; i < F->ns; i++)
        if (F->star[i].color == p->color && dist_approx(p->x - F->star[i].x, p->y - F->star[i].y) < F->star[i].r) k->star[F->star[i].idx].inside++;
}

// One frame of one particle; false when it left the glass, was eaten or grew old (the caller lets it out again).
static bool move_particle(const field_t *F, sky_world_t *k, sky_particle_t *p, int parity, sky_hits_t *hits) {
    int32_t ax, ay;
    accelerate(F, p, &ax, &ay);
    int32_t limit = (int32_t)(SKY_MAX_SPEED * SKY_Q3);
    int32_t vx = clamp32(p->vx + (int32_t)((int64_t)ax * F->dt / 65536), -limit, limit);
    int32_t vy = clamp32(p->vy + (int32_t)((int64_t)ay * F->dt / 65536), -limit, limit);
    int32_t x0 = p->x, y0 = p->y;
    p->vx = (int16_t)vx; p->vy = (int16_t)vy;
    p->x = (int16_t)clamp32(x0 + vx * F->dt / 65536, -4000, 7000);
    p->y = (int16_t)clamp32(y0 + vy * F->dt / 65536, -4000, 7000);
    touch_tools(F, k, p, parity, x0, y0, hits);
    if (k) count_in_stars(F, k, p);
    bool out = p->x < -q3(30) || p->x > q3(SKY_W + 30) || p->y < -q3(30) || p->y > q3(SKY_W + 30);
    return !(out || eaten(F, p) || ++p->age > MAX_AGE);
}

static void update_stars(sky_world_t *k, float dt, sky_hits_t *hits) {
    for (int i = 0; i < k->count_stars; i++) {
        sky_star_t *s = &k->star[i];
        float feed = (float)s->inside * FILL_RATE * k->feed_scale;
        s->fill += (feed - DRAIN) * dt;
        s->fill = s->fill < 0 ? 0 : s->fill > 1 ? 1 : s->fill;
        s->glow *= 1.f - fminf(1.f, 1.8f * dt);
        bool full = s->fill >= SKY_FULL;
        if (full && !s->sang) s->glow = 1.f;
        if (hits) { hits->inside[i] = s->inside; hits->locked |= (uint8_t)(full && !s->sang ? 1u << i : 0u); }
        s->sang = full;
        s->inside = 0;
        if (s->orbit > 0) {
            s->phase += s->spin * dt;
            s->x = s->ax + s->orbit * cosf(s->phase);
            s->y = s->ay + s->orbit * .7f * sinf(s->phase);
        }
    }
    for (int i = 0; i < k->count_tools; i++) k->tool[i].pluck_wait = fmaxf(0, k->tool[i].pluck_wait - dt);
}

void sky_stream_step(sky_world_t *k, sky_particle_t *ps, int n, float dt, const sky_force_t *force, sky_hits_t *hits) {
    if (!(dt > 0)) return;
    field_t F;
    dt = dt > .1f ? .1f : dt;
    load_field(&F, k, dt, force, false);
    for (int i = 0; i < k->count_tools; i++) if (k->tool[i].kind == TOOL_WELL) k->tool[i].held = 0;
    if (hits) memset(hits, 0, sizeof *hits);
    for (int i = 0; i < n; i++) {
        sky_particle_t *p = &ps[i];
        if (p->wait) { p->wait--; continue; }
        if (!move_particle(&F, k, p, i & 1, hits)) respawn(k, p);
    }
    update_stars(k, dt, hits);
    k->t += dt;
}

// Every particle lets go from a place: Tess's own dots when the first sky begins, the source otherwise.
void sky_stream_launch(sky_world_t *k, sky_particle_t *ps, int n, const int32_t from[][2]) {
    for (int i = 0; i < n; i++) {
        sky_particle_t *p = &ps[i];
        respawn(k, p);
        p->wait = 0;
        if (!from) { p->wait = (uint8_t)(sky_rand(k) % 90); continue; }
        float away = sky_range(k, 20.f, 150.f);
        float cx = (float)(from[i][0] - q3(SKY_CX)) / SKY_Q3, cy = (float)(from[i][1] - q3(SKY_CY)) / SKY_Q3;
        float len = hypotf(cx, cy);
        if (len < 1.f) { cx = sky_range(k, -1, 1); cy = sky_range(k, -1, 1); len = hypotf(cx, cy) + .01f; }
        p->x = (int16_t)from[i][0]; p->y = (int16_t)from[i][1];
        p->vx = (int16_t)q3(k->source.dx * k->source.speed * .6f + cx / len * away);
        p->vy = (int16_t)q3(k->source.dy * k->source.speed * .6f + cy / len * away);
        p->age = (uint8_t)(sky_rand(k) % 60);
    }
}

// One light from the source, exactly on the stream's middle, for `steps` frames of 1/30 s: the sky's solution is
// found by following it. It leaves the glass or is eaten: the path ends.
void sky_trace(const sky_world_t *w, int parity, const sky_force_t *force, int steps, int every, sky_path_t *path) {
    field_t F;
    load_field(&F, w, 1.f / 30.f, force, true);
    F.ns = 0;   // the light goes where the tools and the tilt send it: the stars do not bend it, so they can be set on its path
    sky_particle_t p = {.x = (int16_t)q3(w->source.x), .y = (int16_t)q3(w->source.y),
                        .vx = (int16_t)q3(w->source.dx * w->source.speed), .vy = (int16_t)q3(w->source.dy * w->source.speed)};
    path->n = 0;
    uint16_t arc = 0;
    int px = p.x, py = p.y;
    for (int i = 0; i < steps && path->n < SKY_PATH_N; i++) {
        if (!move_particle(&F, NULL, &p, parity, NULL)) break;
        arc = (uint16_t)(arc + dist_approx(p.x - px, p.y - py) / SKY_Q3);
        px = p.x; py = p.y;
        if (p.x < 0 || p.x > q3(SKY_W) || p.y < 0 || p.y > q3(SKY_W)) break;   // it has left the glass
        if (i % every) continue;
        path->x[path->n] = p.x; path->y[path->n] = p.y; path->color[path->n] = p.color; path->arc[path->n++] = arc;
    }
}
