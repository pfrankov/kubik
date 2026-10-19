#include "tess_sky_internal.h"

#include <math.h>
#include <string.h>

// Every moving light uses the same bounded, softened gravitational field. There are no collisions, direction
// overrides or central drag: a hole bends the stream into a fly-by and gives it a little momentum. The integer
// inner loop runs at 60 Hz, carrying unused frame time forward, so rendering cadence cannot change a level.
#define G_PX 300.f
#define HOLE_A 520
#define STAR_A 110
#define BOOST_A 24
#define BOOST_SPEED 220
#define CORE_PX 24
#define FILL_RATE .075f
#define DRAIN .045f
#define MAX_AGE 480

typedef struct { int32_t x, y, r, gain, spin; uint8_t slot; } hole_t;
typedef struct { int32_t x, y; } star_t;
typedef struct {
    int32_t gx, gy;
    int nh, ns;
    hole_t hole[SKY_HUMS];
    star_t star[SKY_STARS];
} field_t;

uint32_t sky_rand(sky_world_t *w) {
    uint32_t v = w->rng ? w->rng : 0x9E3779B9u;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return w->rng = v;
}
float sky_randf(sky_world_t *w) { return (sky_rand(w) & 0xFFFFFFu) / (float)0x1000000; }
float sky_range(sky_world_t *w, float low, float high) { return low + (high - low) * sky_randf(w); }
static int32_t q3(float px) { return (int32_t)lrintf(px * SKY_Q3); }
static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t integrate(int32_t rate, int8_t *remainder) {
    int32_t amount = rate + *remainder;
    *remainder = (int8_t)(amount % 60);
    return amount / 60;
}
// A cheap distance estimate for the smooth force falloff, within 7% of the circular distance.
static int32_t distance(int32_t dx, int32_t dy) {
    int32_t a = dx < 0 ? -dx : dx, b = dy < 0 ? -dy : dy;
    return a > b ? a + ((b * 3) >> 3) : b + ((a * 3) >> 3);
}

bool sky_solved(const sky_world_t *w) {
    if (!w->count_stars) return false;
    for (int i = 0; i < w->count_stars; i++) if (w->star[i].fill < SKY_FULL) return false;
    return true;
}
void sky_solution(const sky_world_t *w, sky_force_t *force) {
    *force = (sky_force_t){.gx = w->gravity[0], .gy = w->gravity[1]};
    if (w->kind == SKY_AURORA) { force->gx = .06f; return; } // keep diagnostic free play active with a gentle real tilt
    if (!w->touch_solution || !w->count_stars) return;
    force->count = 1;
    force->touch[0] = (sky_touch_t){.x = w->star[0].ax, .y = w->star[0].ay, .shown = 1, .spin = 1, .down = true};
}

static void add_hole(field_t *f, float x, float y, float r, float shown, int spin, int slot) {
    if (!(r > 0) || !(shown > 0)) return;
    f->hole[f->nh++] = (hole_t){q3(x), q3(y), q3(r), (int32_t)(fminf(shown, 1.f) * 256.f), spin < 0 ? -1 : 1, (uint8_t)slot};
}
static void load_field(field_t *f, const sky_world_t *w, const sky_force_t *force) {
    memset(f, 0, sizeof *f);
    f->gx = q3(fminf(1.f, fmaxf(-1.f, force->gx)) * G_PX);
    f->gy = q3(fminf(1.f, fmaxf(-1.f, force->gy)) * G_PX);
    for (int i = 0; i < w->count_holes && i < SKY_HOLES; i++) {
        const sky_hole_t *h = &w->hole[i];
        add_hole(f, h->x, h->y, h->r, 1.f, h->spin, i);
    }
    for (int i = 0; i < force->count && i < SKY_TOUCHES; i++) {
        const sky_touch_t *t = &force->touch[i];
        add_hole(f, t->x, t->y, SKY_FINGER_R, t->shown, t->spin, SKY_HOLES + i);
    }
    for (int i = 0; i < w->count_stars && i < SKY_STARS; i++)
        f->star[f->ns++] = (star_t){q3(w->star[i].x), q3(w->star[i].y)};
}

static void gravity(const hole_t *h, const sky_particle_t *p, int32_t *ax, int32_t *ay, sky_hits_t *hits) {
    int32_t dx = h->x - p->x, dy = h->y - p->y, d = distance(dx, dy);
    if (d >= h->r) return;
    int32_t near = (h->r - d) * 256 / h->r;
    int32_t weight = near * near / 256 * h->gain / 256;
    int32_t radial = HOLE_A * SKY_Q3 * weight / 256;
    int32_t curl = radial * h->spin * 7 / 16, soft = d + CORE_PX * SKY_Q3;
    *ax += (radial * dx - curl * dy) / soft;
    *ay += (radial * dy + curl * dx) / soft;
    int32_t speed = distance(p->vx, p->vy);
    if (speed > 0 && speed < BOOST_SPEED * SKY_Q3) {
        int32_t boost = BOOST_A * SKY_Q3 * weight / 256;
        *ax += p->vx * boost / speed; *ay += p->vy * boost / speed;
    }
    if (hits && near > 100 && hits->orbit[h->slot] < 255) hits->orbit[h->slot]++;
}
static void star_pull(const star_t *s, const sky_particle_t *p, int32_t *ax, int32_t *ay) {
    int32_t dx = s->x - p->x, dy = s->y - p->y, d = distance(dx, dy), reach = (int32_t)(SKY_STAR_PULL_R * SKY_Q3);
    if (d >= reach) return;
    int32_t strength = STAR_A * SKY_Q3 * (reach - d) / reach;
    *ax += strength * dx / reach; *ay += strength * dy / reach;
}
static void respawn(sky_world_t *w, sky_particle_t *p) {
    const sky_source_t *s = &w->source;
    float sideways = sky_range(w, -1.f, 1.f), along = sky_range(w, 0.f, 6.f);
    float speed = s->speed * sky_range(w, .92f, 1.08f), jitter = sky_range(w, -.045f, .045f) * speed;
    p->x = (int16_t)q3(s->x - s->dy * sideways * 4.f + s->dx * along);
    p->y = (int16_t)q3(s->y + s->dx * sideways * 4.f + s->dy * along);
    p->vx = (int16_t)q3(s->dx * speed - s->dy * jitter);
    p->vy = (int16_t)q3(s->dy * speed + s->dx * jitter);
    p->color = COL_TEAL; p->age = p->spare = 0;
    if (w->kind == SKY_AURORA) p->color = (uint8_t)(sky_rand(w) % COL_COUNT);
    p->rx = p->ry = p->rvx = p->rvy = 0;
    p->wait = (uint16_t)(sky_rand(w) % 32);
}
static void feed_stars(const field_t *f, sky_world_t *w, sky_particle_t *p) {
    for (int i = 0; i < f->ns; i++) {
        if (distance(p->x - f->star[i].x, p->y - f->star[i].y) >= (int32_t)(SKY_ZONE_R * SKY_Q3)) continue;
        w->star[i].inside++;
        p->color = w->star[i].color; // colour follows a note the light has visited, without restricting later targets
    }
}
static bool move_particle(const field_t *f, sky_particle_t *p, sky_hits_t *hits) {
    int32_t ax = f->gx, ay = f->gy;
    for (int i = 0; i < f->nh; i++) gravity(&f->hole[i], p, &ax, &ay, hits);
    for (int i = 0; i < f->ns; i++) star_pull(&f->star[i], p, &ax, &ay);
    int32_t vx = p->vx + integrate(ax, &p->rvx);
    int32_t vy = p->vy + integrate(ay, &p->rvy);
    int32_t avx = vx < 0 ? -vx : vx, avy = vy < 0 ? -vy : vy;
    int32_t speed = avx > avy ? avx + avy / 2 : avy + avx / 2;
    int32_t limit = (int32_t)(SKY_MAX_SPEED * SKY_Q3); // This norm always bounds the true diagonal speed.
    if (speed > limit) { vx = vx * limit / speed; vy = vy * limit / speed; }
    p->vx = (int16_t)vx; p->vy = (int16_t)vy;
    p->x = (int16_t)clamp32(p->x + integrate(vx, &p->rx), -4000, 7000);
    p->y = (int16_t)clamp32(p->y + integrate(vy, &p->ry), -4000, 7000);
    bool out = p->x < -30 * SKY_Q3 || p->x > (SKY_W + 30) * SKY_Q3 || p->y < -30 * SKY_Q3 || p->y > (SKY_W + 30) * SKY_Q3;
    return !out && ++p->age <= MAX_AGE;
}
static void update_stars(sky_world_t *w, sky_hits_t *hits) {
    for (int i = 0; i < w->count_stars; i++) {
        sky_star_t *s = &w->star[i];
        float feed = (float)s->inside * FILL_RATE * w->feed_scale;
        bool was_full = s->fill >= SKY_FULL;
        s->fill = was_full ? 1.f : fminf(1.f, fmaxf(0, s->fill + (feed - DRAIN) * SKY_STEP_S));
        s->glow *= 1.f - 1.8f * SKY_STEP_S;
        if (s->fill >= SKY_FULL && !s->sang) {
            s->fill = 1.f; s->glow = 1.f; s->sang = true;
            hits->locked |= (uint8_t)(1u << i);
        }
        hits->inside[i] = s->inside;
        s->inside = 0;
        if (s->orbit <= 0) continue;
        s->phase += s->spin * SKY_STEP_S;
        s->x = s->ax + s->orbit * cosf(s->phase);
        s->y = s->ay + s->orbit * .7f * sinf(s->phase);
    }
}
static void tick(sky_world_t *w, sky_particle_t *ps, int n, const sky_force_t *force, sky_hits_t *hits) {
    field_t f;
    load_field(&f, w, force);
    for (int i = 0; i < n; i++) {
        sky_particle_t *p = &ps[i];
        if (p->wait) { p->wait--; continue; }
        if (!move_particle(&f, p, hits)) respawn(w, p);
        else feed_stars(&f, w, p);
    }
    update_stars(w, hits);
    w->t += SKY_STEP_S;
}
static void publish_hits(sky_hits_t *hits, unsigned ticks, const unsigned inside[], const unsigned orbit[]) {
    if (hits) {
        for (int i = 0; i < SKY_STARS; i++) hits->inside[i] = (uint8_t)(inside[i] / ticks);
        for (int i = 0; i < SKY_HUMS; i++) hits->orbit[i] = (uint8_t)(orbit[i] / ticks);
    }
}
void sky_stream_step(sky_world_t *w, sky_particle_t *ps, int n, float dt, const sky_force_t *force, sky_hits_t *hits) {
    if (hits) memset(hits, 0, sizeof *hits);
    if (!(dt > 0) || !isfinite(dt)) return;
    w->remainder += fminf(dt, .1f);
    unsigned ticks = 0, inside[SKY_STARS] = {0}, orbit[SKY_HUMS] = {0};
    while (w->remainder + .000001f >= SKY_STEP_S && ticks < 6) {
        sky_hits_t step = {0};
        tick(w, ps, n, force, &step);
        if (hits) hits->locked |= step.locked;
        for (int i = 0; i < SKY_STARS; i++) inside[i] += step.inside[i];
        for (int i = 0; i < SKY_HUMS; i++) orbit[i] += step.orbit[i];
        w->remainder = fmaxf(0, w->remainder - SKY_STEP_S);
        ticks++;
    }
    if (ticks) publish_hits(hits, ticks, inside, orbit);
}

void sky_stream_launch(sky_world_t *w, sky_particle_t *ps, int n, const int32_t from[][2]) {
    for (int i = 0; i < n; i++) {
        sky_particle_t *p = &ps[i];
        respawn(w, p);
        p->wait = 0;
        if (!from) { p->wait = (uint16_t)(sky_rand(w) % 120); continue; }
        float away = sky_range(w, 20.f, 150.f);
        float cx = (float)(from[i][0] - q3(SKY_CX)) / SKY_Q3, cy = (float)(from[i][1] - q3(SKY_CY)) / SKY_Q3;
        float len = hypotf(cx, cy);
        if (len < 1.f) { cx = 1; cy = 0; len = 1; }
        p->x = (int16_t)from[i][0]; p->y = (int16_t)from[i][1];
        p->vx = (int16_t)q3(w->source.dx * w->source.speed * .6f + cx / len * away);
        p->vy = (int16_t)q3(w->source.dy * w->source.speed * .6f + cy / len * away);
        p->age = (uint16_t)(sky_rand(w) % 120);
    }
}
void sky_trace(const sky_world_t *w, const sky_force_t *force, int steps, int every, sky_path_t *path) {
    field_t f;
    load_field(&f, w, force);
    f.ns = 0; // Place targets where the field leads, before the targets themselves attract the stream.
    sky_particle_t p = {.x = (int16_t)q3(w->source.x), .y = (int16_t)q3(w->source.y),
        .vx = (int16_t)q3(w->source.dx * w->source.speed), .vy = (int16_t)q3(w->source.dy * w->source.speed)};
    path->n = 0;
    if (every < 1) return;
    uint16_t arc = 0;
    int px = p.x, py = p.y;
    for (int i = 0; i < steps && path->n < SKY_PATH_N; i++) {
        if (!move_particle(&f, &p, NULL)) break;
        arc = (uint16_t)(arc + distance(p.x - px, p.y - py));
        px = p.x; py = p.y;
        if (i % every) continue;
        path->x[path->n] = p.x; path->y[path->n] = p.y;
        path->color[path->n] = p.color; path->arc[path->n++] = arc / SKY_Q3;
    }
}
