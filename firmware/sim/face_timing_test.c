// clang -std=c11 -Wall -Wextra -fsanitize=address,undefined -g -O1 firmware/sim/face_timing_test.c firmware/main/face.c firmware/main/render.c firmware/main/render_dots.c firmware/main/render_glass.c firmware/main/render_output.c firmware/main/render_pipeline.c firmware/main/render_raster.c firmware/main/render_scene.c firmware/main/render_text.c firmware/main/sprite.c firmware/main/font.c firmware/main/font_data.c -lm -o /tmp/c6-face-timing-test && /tmp/c6-face-timing-test
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "../main/face.h"
static unsigned pushes;
static void push(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    (void)ctx; (void)px;
    assert(x0 >= 0 && y0 >= 0 && x1 <= R_W && y1 <= R_H);
    assert(x1 > x0 && y1 > y0);
    pushes++;
}
int main(void) {
    face_t f; face_init(&f);
    render_state_t st = {0};
    uint16_t band[R_W * R_BAND], *bufs[] = {band};
    const float times[] = {.1f, .033f, .066f, .02f};
    float elapsed = 0;
    for (int i = 0; elapsed < 30; i++) {
        float dt = times[i % 4]; elapsed += dt;
        if (i % 17 == 0) face_set_emotion(&f, (emotion_t)((i / 17) % EMO_COUNT), -1);
        if (i % 31 == 0) face_event(&f, FEV_SHAKE, 0, 0);
        f.tilt_x = sinf(elapsed * 2); f.tilt_y = cosf(elapsed);
        face_update(&f, dt);
        assert(fabsf(f.t - elapsed) < .001f);
        for (int j = 0; j < FACE_NP; j++) {
            assert(isfinite(f.cur.v[j]) && fabsf(f.cur.v[j]) < 1000);
            assert(isfinite(f.vel.v[j]) && fabsf(f.vel.v[j]) < 10000);
        }
        assert(isfinite(f.hop) && fabsf(f.hop) < 100);
        assert(isfinite(f.hop_v) && fabsf(f.hop_v) < 1000);
        scene_t s; face_draw(&f, &s);
        render_frame(&st, &s, bufs, 1, false, push, NULL);
    }
    assert(pushes > 100);
    puts("variable frame time: 30s finite/bounded springs, hop, elapsed time and rendering passed");
}
