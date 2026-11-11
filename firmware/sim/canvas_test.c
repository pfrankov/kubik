// clang -std=c11 -Wall -Wextra -fsanitize=address,undefined -g -O1 firmware/sim/canvas_test.c firmware/main/canvas.c -o /tmp/c6-canvas-test && /tmp/c6-canvas-test
#include "../main/canvas.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static canvas_t c;
static uint16_t frame[CANVAS_H][CANVAS_W];
static uint32_t reference_stripe(const uint16_t *pixels) {
    uint32_t hash=0x811C9DC5u,any=0;
    for(int i=0;i<CANVAS_STRIPE;i+=2) {
        uint32_t word=pixels[i]|((uint32_t)pixels[i+1]<<16);
        any|=word;hash=(hash^word)*0x9E3779B1u;hash^=hash>>15;
    }
    return any?hash:0xF0229BE3u;
}
// Feeds a frame in 8-row bands (or unaligned, to cover the pairwise reads) and returns its changed rows' bounds.
static canvas_rect_t build(bool unaligned) {
    static uint16_t shifted[CANVAS_W * 8 + 1];
    canvas_rect_t r = {CANVAS_W, CANVAS_H, 0, 0};
    for (int y = 0; y < CANVAS_H; y += 8) {
        const uint16_t *src = frame[y];
        if (unaligned) { memcpy(shifted + 1, frame[y], sizeof(frame[0]) * 8); src = shifted + 1; }
        assert(canvas_rows(&c, y, y + 8, src));
    }
    for (int y = 0; y < CANVAS_H; y++) {
        uint16_t lit = 0;
        for (int x = 0; x < CANVAS_W; x++) if (frame[y][x]) lit |= 1u << (x / CANVAS_STRIPE);
        assert(c.row_lit[y] == lit);
        for (int k = 0; k < CANVAS_STRIPES; k++)
            assert(c.stripe_hash[y][k] == reference_stripe(frame[y]+k*CANVAS_STRIPE));
        if (c.changed_x0[y] >= c.changed_x1[y]) continue;
        if (c.changed_x0[y] < r.x0) r.x0 = c.changed_x0[y];
        if (c.changed_x1[y] > r.x1) r.x1 = c.changed_x1[y];
        if (y < r.y0) r.y0 = y;
        r.y1 = y + 1;
    }
    return r;
}
static void rect(canvas_rect_t r, int x0, int y0, int x1, int y1) {
    if (!(r.x0 == x0 && r.y0 == y0 && r.x1 == x1 && r.y1 == y1)) {
        printf("got %d,%d..%d,%d want %d,%d..%d,%d\n", r.x0, r.y0, r.x1, r.y1, x0, y0, x1, y1);
        abort();
    }
}
static bool unchanged(canvas_rect_t r) { return r.x0 >= r.x1 && r.y0 >= r.y1; }
static void fold_collision(void) {
    memset(&c,0,sizeof c);memset(frame,0,sizeof frame);build(false);
    // Two changed stripe hashes with the exact same ordered row fold.
    const uint32_t seed=0x811C9DC5u,multiplier=0x9E3779B1u,zero=0xF0229BE3u;
    uint32_t first=(seed^zero)*multiplier;
    c.stripe_hash[10][0]=zero^1u;
    c.stripe_hash[10][1]=((seed^c.stripe_hash[10][0])*multiplier)^first^zero;
    assert(unchanged(build(false)) && c.changed_mask[10]==0);
    frame[10][5]=0x1234;rect(build(false),5,10,6,11);
}
int main(void) {
    for (int pass = 0; pass < 2; pass++) {
        bool u = pass;
        memset(&c, 0, sizeof c); memset(frame, 0, sizeof frame);
        rect(build(u), 0, 0, 240, 240);  // no previous frame: everything
        assert(unchanged(build(u)));
        frame[10][5] = 0xffff; frame[10][8] = 0x1234;
        rect(build(u), 5, 10, 9, 11);    // exactly the lit pixels
        frame[10][5] = 0; frame[10][8] = 0; frame[12][2] = 1;
        rect(build(u), 2, 10, 9, 13);    // includes cleared old pixels
        frame[12][2] = 0; rect(build(u), 2, 12, 3, 13);
        for (int x = 0; x < 240; x++) frame[20][x] = (uint16_t)(x + 1);
        rect(build(u), 0, 20, 240, 21);
        // A lit row changing in one stripe is sent across that stripe only.
        frame[20][100] ^= 0x0800;
        rect(build(u), 96, 20, 112, 21);
        assert(unchanged(build(u)));
        canvas_reset(&c);                // the panel lost the picture: everything again
        rect(build(u), 0, 0, 240, 240);
        assert(unchanged(build(u)));
    }
    // Random edits: every changed pixel lies in its row's changed span.
    static uint16_t prev[CANVAS_H][CANVAS_W];
    srand(7);
    for (int round = 0; round < 300; round++) {
        memcpy(prev, frame, sizeof frame);
        for (int e = rand() % 20; e > 0; e--) {
            int y = rand() % 240, x = rand() % 240, n = 1 + rand() % (240 - x), kind = rand() % 3;
            for (int i = 0; i < n; i++) frame[y][x + i] = kind == 0 ? 0 : kind == 1 ? 0x7777 : (uint16_t)(rand() | 1);
        }
        build(round & 1);
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 240; x++)
                if (frame[y][x] != prev[y][x]) assert(x >= c.changed_x0[y] && x < c.changed_x1[y]);
    }
    assert(!canvas_rows(&c, 0, 1, NULL)); assert(!canvas_rows(&c, 239, 241, frame[0])); assert(!canvas_rows(NULL, 0, 1, frame[0]));
    fold_collision();
    printf("canvas: exact changed spans (aligned and not), stripe narrowing, reset and random edits passed; sizeof=%zu\n", sizeof c);
}
