// clang -std=c11 -fsanitize=address,undefined -g -O1 firmware/sim/sprite_interpolation_test.c firmware/main/render.c firmware/main/render_dots.c firmware/main/render_glass.c firmware/main/render_output.c firmware/main/render_pipeline.c firmware/main/render_raster.c firmware/main/render_scene.c firmware/main/render_text.c firmware/main/face.c -lm -o /tmp/c6-sprite-test && /tmp/c6-sprite-test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/sprite.c"
#include "../main/face.h"
static uint8_t pack[1024];
static unsigned fetches;
static const uint8_t *fetch(uint32_t off, uint32_t len, uint32_t ho, uint32_t hl) {
    (void)ho; (void)hl; fetches++;
    assert(off + len <= sizeof pack);
    return pack + off;
}
static void fixture(void) {
    memset(pack, 0, sizeof pack);
    memcpy(pack, "KSPR", 4);
    pack[4] = 5; pack[6] = 1; pack[8] = 2; pack[12] = 240;
    uint16_t *pal = (uint16_t *)(pack + HDR);
    pal[1] = 0xf800; pal[2] = 0x001f;
    sprite_anim_t *an = (sprite_anim_t *)(pack + HDR + 512);
    strcpy(an->name, "listen"); an->count = 2; an->fps = 10;
    an->loop_a = 0; an->loop_b = 1;
    frame_t *fr = (frame_t *)(pack + HDR + 512 + ASZ);
    for (int i = 0; i < 2; i++) {
        int x = 2 + i * 3;
        fr[i].off = 700 + i * 32; fr[i].len = 14;
        fr[i].x0 = x; fr[i].x1 = x; fr[i].cx4 = 160 + i * 160; fr[i].vis = 255;
        uint8_t data[] = {0,2,0,0,4,0,1,(uint8_t)x,1,(uint8_t)(i+1),1,(uint8_t)x,1,(uint8_t)(2-i)};
        memcpy(pack + fr[i].off, data, sizeof data);
    }
    assert(sprite_init(fetch));
}
// Match the renderer's single conversion from panel to framebuffer coordinates.
static void native(prim_t *p) {
    p->cx /= 2; p->cy /= 2;
    p->bx0 /= 2; p->bx1 /= 2; p->by0 /= 2; p->by1 /= 2;
}
static void draw(float pos, bool swap, bool opaque, uint16_t *out) {
    scene_t s; scene_begin(&s, 0);
    prim_t *p = sc_sprite(&s, 0, pos, 0, 0, 1);
    assert(p && s.n == 1); native(p);
    g_render_swap = swap; g_render_img_opaque_ok = opaque;
    memset(out, 0, 16 * sizeof *out);
    unsigned before = fetches;
    raster(p, out, 0, 8, 0, 1, 0);
    assert(fetches - before == (p->r ? 2u : 1u));
}
int main(void) {
    fixture();
    uint16_t a[16], b[16], mid[16], out[16];
    for (int sw = 0; sw < 2; sw++) for (int op = 0; op < 2; op++) {
        draw(0, sw, op, a); draw(1, sw, op, b); draw(.5f, sw, op, mid);
        // One opaque pose at every timestamp: never two separated silhouettes.
        assert(!memcmp(mid, b, sizeof mid));
        draw(.49f, sw, op, out); assert(!memcmp(out, a, sizeof out));
        draw(.99f, sw, op, out); assert(!memcmp(out, b, sizeof out));
        draw(1.75f, sw, op, out); assert(!memcmp(out, b, sizeof out));
        draw(-1, sw, op, out); assert(!memcmp(out, a, sizeof out));
    }
    scene_t scene; scene_begin(&scene, 0);
    prim_t *p = sc_sprite(&scene, 0, .5f, 0, 0, 1);
    assert(p->bx0 == 10 && p->bx1 == 11); native(p);
    for (int sw = 0; sw < 2; sw++) for (int op = 0; op < 2; op++) {
        g_render_swap = sw; g_render_img_opaque_ok = op;
        for (int j = 0; j < 16; j++) out[j] = 0xffff;
        raster(p, out, 0, 8, 0, 1, 0);
        assert(out[4] == 0xffff); // outside the selected pose is untouched
        assert(out[0] == 0xffff); // outside union untouched
    }
    sprite_screen_t sc; assert(sprite_screen(0, .5f, &sc)); assert(sc.cx == 20.f);
    assert(sprite_screen(0, 1.5f, &sc)); assert(sc.cx == 20.f);
    face_t f; face_init(&f); face_set_mode(&f, MODE_LISTENING);
    const sprite_anim_t *an = sprite_anim(f.body.id[BA_LISTEN]);
    f.body.req = -1; f.body.cur = BA_LISTEN; f.body.pos = an->loop_a; f.body.dir = 1; f.body.vel = 0;
    f.body.oneshot = false; f.body.leaving = false;
    // The hold swings between loop_a and loop_b without a jump in speed (eased turns).
    float lo = 1e9f, hi = -1e9f, v0 = 0, max_dv = 0;
    for (int i = 0; i < 400; i++) {
        face_update(&f, .01f);
        if (f.body.cur != BA_LISTEN) break;
        lo = fminf(lo, f.body.pos); hi = fmaxf(hi, f.body.pos);
        max_dv = fmaxf(max_dv, fabsf(f.body.vel - v0)); v0 = f.body.vel;
    }
    assert(f.body.cur == BA_LISTEN);
    assert(hi <= an->loop_b + .05f && hi > an->loop_b - .5f && lo >= an->loop_a - .05f);
    assert(max_dv <= an->fps * 3.5f * .01f + 1e-3f);
    face_set_mode(&f, MODE_SETUP); f.body.cur = -1; f.body.pos = .8f; f.body.req = -1;
    body_t hidden = f.body;
    face_update(&f, .03f); assert(!memcmp(&f.body, &hidden, sizeof hidden));
    fixture(); draw(.5f, false, true, out);
    for (int version = 2; version <= 4; version++) {
        pack[4] = version; assert(!sprite_init(fetch)); assert(!sprite_ready());
    }
    pack[0] = 0; assert(!sprite_init(fetch)); assert(!sprite_ready());
    puts("native sprite: endpoints, opaque pose selection, bbox, byte order, overlay, ping-pong, elapsed time and rest passed");
}
