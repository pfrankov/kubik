// Waiting approaches; speech stays large and follows playback; return is continuous.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_draw.c"
#define DT (1.f / 30)

static face_t start(void) {
    face_t f; face_init(&f); face_set_character(&f, CHARACTER_TESS);
    for (int i = 0; i < 90; i++) face_update(&f, DT);
    return f;
}
static float radius(face_t *f) {
    float points[TESS_N][3], total = 0;
    tess_point_targets(f, points, -1);
    for (int i = 0; i < TESS_N; i++)
        for (int k = 0; k < 3; k++) total += points[i][k] * points[i][k];
    return sqrtf(total / TESS_N);
}
static void check_size_and_color(void) {
    face_t idle = start(), wait = idle, reply = idle;
    wait.tess_mode[TM_THINK] = 1; wait.tess_xw = -.35f; // same 4D pose isolates size
    reply.tess_mode[TM_SPEAK] = 1;
    float normal = radius(&idle), thinking = radius(&wait), silent = radius(&reply);
    uint32_t quiet_color = tess_color(&reply, .9f);
    reply.tess_audio = 1;
    float loud = radius(&reply);
    printf("conversation size: idle %.3f, waiting %.3f, reply silence %.3f, voiced %.3f\n", normal, thinking, silent, loud);
    assert(thinking > normal * 1.25f && silent > thinking && loud > silent * 1.04f);
    assert(tess_color(&reply, .9f) != quiet_color);
    assert(reply_tint(&reply) <= .30f); // voice remains a small tint, not a palette replacement
}
static float turn_speed(float level) {
    face_t f = start(); face_set_mode(&f, MODE_SPEAKING);
    for (int i = 0; i < 120; i++) { f.spk_level = level; face_update(&f, DT); }
    float before = f.tess_xw; face_update(&f, DT);
    float turn = f.tess_xw - before;
    return (turn < 0 ? turn + 2 * PI : turn) / DT;
}
static void check_turn_and_silence(void) {
    float quiet = turn_speed(0), loud = turn_speed(.8f);
    assert(quiet > .2f && loud > quiet * 2.f && loud < 1.3f);
    face_t f = start(); face_set_mode(&f, MODE_SPEAKING);
    for (int i = 0; i < 60; i++) { f.spk_level = 1; face_update(&f, DT); }
    f.spk_level = 0;
    for (int i = 0; i < 30; i++) face_update(&f, DT);
    assert(f.tess_audio < .01f);
    printf("reply 4D: silence %.3f, voiced %.3f rad/s; silence envelope %.5f\n", quiet, loud, f.tess_audio);
}
static void check_transition(face_t *f, face_mode_t mode, float dt) {
    float before[TESS_N][3]; memcpy(before, f->tess_position, sizeof before);
    face_set_mode(f, mode);
    assert(!memcmp(before, f->tess_position, sizeof before)); // mode changes never reset points
    for (int i = 0; i < 150; i++) {
        memcpy(before, f->tess_position, sizeof before);
        f->spk_level = i % 40 < 12 ? 0 : .15f + .8f * fabsf(tess_sin(i * .23f));
        face_update(f, dt);
        scene_t s; scene_begin(&s, 0); tess_draw(f, &s);
        assert(s.dots_n == TESS_N + (!f->live_active && f->tess_record > .01f ? 18 : 0));
        for (int p = 0; p < TESS_N; p++) {
            for (int k = 0; k < 3; k++) {
                assert(isfinite(f->tess_position[p][k]));
                assert(fabsf(f->tess_position[p][k] - before[p][k]) < 30.f * dt);
            }
            if (f->tess_mode[TM_THINK] + f->tess_mode[TM_SPEAK] >= .25f) {
                const render_dot_t *dot = &s.dots[p];
                assert(dot->x - dot->radius * 3 >= 24 * 8 - 3);
                assert(dot->x + dot->radius * 3 <= 456 * 8 + 3);
                assert(dot->y - dot->radius * 3 >= 57 * 8 - 3);
                assert(dot->y + dot->radius * 3 <= (f->live_active ? 395 : 453) * 8 + 3);
            }
        }
    }
}
static void check_interrupted_conversation(void) {
    const float dt[] = {DT, .027f, .040f};
    for (int n = 0; n < 3; n++) {
        face_t f = start();
        check_transition(&f, MODE_THINKING, dt[n]);
        check_transition(&f, MODE_SPEAKING, dt[n]);
        check_transition(&f, MODE_THINKING, dt[n]); // rebuffer while a reply is still arriving
        check_transition(&f, MODE_SPEAKING, dt[n]);
        check_transition(&f, MODE_IDLE, dt[n]);
        assert(f.tess_mode[TM_THINK] + f.tess_mode[TM_SPEAK] < .001f);
    }
    puts("conversation: bounded halos, 4D, preserved positions, interrupted turns and 27/33/40ms frames passed");
}
static void check_live_cube(void) {
    face_t f = start(), reference = f;
    f.live_active = f.live_ready = f.live_mic = true;
    f.tess_mode[TM_LISTEN] = 1;
    float cube[TESS_N][3], idle[TESS_N][3];
    tess_point_targets(&f, cube, -1); tess_point_targets(&reference, idle, -1);
    for (int p = 0; p < TESS_N; p++) for (int k = 0; k < 3; k++)
        assert(fabsf(cube[p][k] - idle[p][k]) < .001f); // Live never morphs into the recording sphere.
    float quiet = radius(&f); f.tess_audio = .8f;
    assert(radius(&f) > quiet * 1.035f); // real microphone envelope moves the rigid body
    f.tess_record = 1;
    scene_t scene; scene_begin(&scene, 0); tess_draw(&f, &scene);
    assert(scene.dots_n == TESS_N); // Live uses the persistent Listening badge.
    f.live_active = false; scene_begin(&scene, 0); tess_draw(&f, &scene);
    assert(scene.dots_n == TESS_N + 18);
    for (int i = TESS_N; i < scene.dots_n; i++) assert(scene.dots[i].color == rgb565(0xFF3B30));
    f.live_active = true;
    check_transition(&f, MODE_LISTENING, DT);
    check_transition(&f, MODE_THINKING, DT);
    check_transition(&f, MODE_SPEAKING, DT);
    puts("Live: cube geometry, microphone reaction and continuous transitions passed");
}
int main(void) {
    check_size_and_color(); check_turn_and_silence(); check_interrupted_conversation(); check_live_cube();
    return 0;
}
