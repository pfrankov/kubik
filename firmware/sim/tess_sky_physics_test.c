// Public stream kernel: contact continuity, energy, smooth wells and elapsed-time drag.
#include "../main/tess_sky_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static sky_world_t world(void) { return (sky_world_t){.feed_scale = 1, .rng = 17}; }
static sky_particle_t light(float x, float y, float vx, float vy) {
    return (sky_particle_t){.x = (int16_t)(x * SKY_Q3), .y = (int16_t)(y * SKY_Q3),
        .vx = (int16_t)(vx * SKY_Q3), .vy = (int16_t)(vy * SKY_Q3)};
}
static sky_tool_t string(float x) { return (sky_tool_t){.kind = TOOL_STRING, .x = x, .y = 240, .dy = 80}; }
static void step(sky_world_t *w, sky_particle_t *p, float dt, sky_hits_t *hits) {
    sky_force_t force = {0}; sky_stream_step(w, p, 1, dt, &force, hits);
}
static void strings_continue_after_contact(void) {
    for (int side = -1; side <= 1; side += 2) {
        sky_world_t w = world(); w.count_tools = 1; w.tool[0] = string(240);
        sky_particle_t p = light(240 + side * 5, 240, -side * 300, 80);
        sky_hits_t hits; step(&w, &p, 1.f / 30, &hits);
        assert(fabsf(p.x / 8.f - (240 + side * 5)) < .5f); // remaining half-frame is travelled
        assert(p.vx * side > 0 && abs(p.vx) < 2400);
        assert(p.vy == 640 && fabsf(p.y / 8.f - 242.6667f) < .3f); // tangent is preserved
        assert(hits.strike[0] > .99f);
    }
    sky_world_t w = world(); w.count_tools = 1; w.tool[0] = string(240);
    sky_particle_t p = light(235, 330, 300, 0); step(&w, &p, 1.f / 30, NULL);
    assert(p.x > 244 * 8 && p.vx == 2400); // beyond the endpoint: no invisible wall
    p = light(235, 240, -300, 0); step(&w, &p, 1.f / 30, NULL);
    assert(p.x < 226 * 8 && p.vx == -2400); // moving away: no collision
}
static void nearest_string_wins(void) {
    sky_world_t w = world(); w.count_tools = 2; w.tool[0] = string(244); w.tool[1] = string(240);
    sky_particle_t p = light(235, 240, 300, 0); sky_hits_t hits;
    step(&w, &p, 1.f / 30, &hits);
    assert(p.x < 240 * 8 && hits.strike[1] > .99f && hits.strike[0] == 0);
    w = world(); w.count_tools = 2; w.tool[0] = string(244); w.tool[1] = string(236);
    p = light(240, 240, 540, 0); int before = abs(p.vx);
    step(&w, &p, 1.f / 30, &hits);
    assert(p.x > 236 * 8 && p.x < 244 * 8 && abs(p.vx) <= before);
    assert(hits.strike[0] > 0 && hits.strike[1] > 0); // both contacts in one frame, bounded to two
}
static void close_strings_cannot_exhaust_the_budget_and_tunnel(void) {
    sky_world_t w = world(); w.count_tools = 2; w.tool[0] = string(239); w.tool[1] = string(240);
    sky_particle_t p = light(239.5f, 240, 460, 0);
    step(&w, &p, 1.f / 30, NULL);
    assert(p.x > 239 * 8 && p.x < 240 * 8 && abs(p.vx) <= 460 * 8);
}
static void splitters_follow_the_reflected_path(void) {
    sky_world_t w = world(); w.count_tools = 2; w.tool[0] = string(240); w.tool[1] = string(238);
    w.tool[1].kind = TOOL_SPLIT;
    sky_particle_t p[2] = {light(235, 240, 300, 0), light(235, 240, 300, 0)};
    sky_force_t force = {0}; sky_hits_t hits;
    sky_stream_step(&w, p, 2, 1.f / 30, &force, &hits);
    assert(hits.strike[0] > .9f);
    assert(p[0].x < 238 * 8 && p[1].x < 238 * 8);
    assert(p[0].vy < -16 && p[0].vy > -64 && p[1].vy == -p[0].vy); // splitter crossed both inbound and outbound
    w.count_tools = 1; w.tool[0] = w.tool[1]; p[0] = light(235, 240, 300, 0);
    step(&w, p, 1.f / 30, NULL);
    assert(p[0].x > 241 * 8 && p[0].y < 239 * 8); // deflection uses the remaining time immediately
}
static sky_world_t well(float radius) {
    sky_world_t w = world(); w.count_tools = 1;
    w.tool[0] = (sky_tool_t){.kind = TOOL_WELL, .x = 240, .y = 240, .r = radius}; return w;
}
static void wells_have_no_central_kick(void) {
    sky_world_t w = well(124); sky_particle_t p = light(240, 240, 0, 0);
    step(&w, &p, 1.f / 30, NULL); assert(!p.vx && !p.vy);
    int speed[2];
    for (int side = -1; side <= 1; side += 2) {
        w = well(124); p = light(240 + side, 240, 0, 0);
        step(&w, &p, 1.f / 30, NULL);
        assert(p.vx * side < 0 && abs(p.vx) <= 8 && p.vy == 0); // under 1 px/s, formerly ~49
        speed[(side + 1) / 2] = abs(p.vx);
    }
    assert(speed[0] == speed[1]);
    w = well(124); p = light(240 + 124, 240, 0, 0); step(&w, &p, 1.f / 30, NULL);
    assert(!p.vx && !p.vy); // rim: no force discontinuity
}
static void wells_bend_a_tangential_light_without_reversing_its_orbit(void) {
    sky_world_t w = well(124); sky_particle_t p = light(150, 240, 0, 100);
    for (int frame = 0; frame < 60; frame++) {
        int x = p.x, y = p.y; step(&w, &p, 1.f / 30, NULL);
        int dx = p.x - 240 * 8, dy = p.y - 240 * 8;
        assert((int64_t)dx * p.vy - (int64_t)dy * p.vx < 0); // keeps circling in the incident direction
        assert(hypotf(dx, dy) < 124 * 8 && hypotf(p.vx, p.vy) < 360 * 8);
        assert(hypotf(p.x - x, p.y - y) < 12 * 8); // no central teleport or impulse
    }
}
static void well_drag_uses_elapsed_time(void) {
    sky_world_t a = well(1024), b = a;
    sky_particle_t p = light(240, 240, 100, 0), q = p;
    step(&a, &p, 1.f / 30, NULL); step(&b, &q, 1.f / 60, NULL); step(&b, &q, 1.f / 60, NULL);
    assert(abs(p.vx - q.vx) <= 2);
    a = well(1024); b = a; p = light(240, 240, 100, 0); q = p;
    for (int i = 0; i < 30; i++) step(&a, &p, 1.f / 30, NULL);
    for (int i = 0; i < 60; i++) step(&b, &q, 1.f / 60, NULL);
    assert(abs(p.vx - q.vx) <= 24 && abs(p.x - q.x) <= 32);
    a = well(124); p = light(240, 240, 100, 0); sky_world_t before = a; sky_particle_t original = p;
    step(&a, &p, 0, NULL); assert(!memcmp(&a, &before, sizeof a) && !memcmp(&p, &original, sizeof p));
}
int main(void) {
    strings_continue_after_contact(); nearest_string_wins(); close_strings_cannot_exhaust_the_budget_and_tunnel(); splitters_follow_the_reflected_path(); wells_have_no_central_kick(); wells_bend_a_tangential_light_without_reversing_its_orbit(); well_drag_uses_elapsed_time();
    puts("sky physics: swept continuation, nearest/multiple contacts, normal restitution, smooth wells and elapsed-time drag passed");
}
