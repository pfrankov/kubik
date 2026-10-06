// The closed-form outline path (raster_frame) must match the generic sphere tracer. Pixels whose distance sits
// within integer-sqrt rounding of a coverage step may differ by a couple of levels; nothing else may.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/render.h"

static uint16_t img[R_H][R_W];
static void push(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    for (int y = y0; y < y1; y++) memcpy(&img[y][x0], px + (y - y0) * (x1 - x0), (size_t)(x1 - x0) * 2);
}
static uint16_t b0[R_W * R_BAND], b1[R_W * R_BAND];
static void draw(float cx, float cy, float hw, float hh, float r, float w, float alpha, bool generic, uint16_t out[R_H][R_W]) {
    // w = 0: a plain box (sharp ones take raster_rect)
    static scene_t s;
    render_state_t rs = {0};
    uint16_t *bufs[2] = {b0, b1};
    scene_begin(&s, 0x0841);
    sc_rbox(&s, 240, 240, 200, 120, 30, 0.3f, 0x3366ff, 1);  // something underneath for blending
    prim_t *p = sc_rbox(&s, cx, cy, hw, hh, r, 0, 0xff2020, alpha);
    if (w > 0) pr_stroke(p, w);
    if (generic) {  // a cut that never cuts forces the generic tracer; distances are unchanged
        p->ncut = 1; p->cut_nx[0] = 16384; p->cut_ny[0] = 0; p->cut_c[0] = 1 << 24;
    }
    render_frame(&rs, &s, bufs, 2, false, push, NULL);
    memcpy(out, img, sizeof img);
}
static int far(uint16_t u, uint16_t v) {
    int dr = abs((u >> 11) - (v >> 11)), dg = abs(((u >> 5) & 63) - ((v >> 5) & 63)), db = abs((u & 31) - (v & 31));
    return dr > 2 || dg > 4 || db > 2;
}
typedef struct { float cx, cy, hw, hh, r, w, alpha; } case_t;
static case_t make_case(int i) {
    case_t c;
    c.cx=240+(i?(rand()%2000-1000)/37.f:0); c.cy=240+(i?(rand()%2000-1000)/41.f:0);
    c.hw=i?40+rand()%220:240; c.hh=i?40+rand()%220:240;
    float mn=c.hw<c.hh?c.hw:c.hh;
    c.r=i?(rand()%1000)/1000.f*mn:84; c.w=i?1+(rand()%400)/10.f:14; c.alpha=(i%3)?1:.55f;
    if(i%4==3) { c.r=0; c.w=0; }
    return c;
}
static int compare_pixels(const uint16_t a[R_H][R_W], const uint16_t b[R_H][R_W]) {
    int diff=0;
    for(int y=0;y<R_H;y++) for(int x=0;x<R_W;x++) diff+=far(a[y][x],b[y][x]);
    return diff;
}
static void test_case(int i, uint16_t a[R_H][R_W], uint16_t b[R_H][R_W], int *bad) {
    case_t c=make_case(i);
    draw(c.cx,c.cy,c.hw,c.hh,c.r,c.w,c.alpha,false,a);
    draw(c.cx,c.cy,c.hw,c.hh,c.r,c.w,c.alpha,true,b);
    int diff=compare_pixels(a,b);
    if(diff) {
        (*bad)++;
        if(*bad<6) printf("case %d: cx %.2f cy %.2f hw %.0f hh %.0f r %.1f w %.1f a %.2f: %d px differ\n",
                          i,c.cx,c.cy,c.hw,c.hh,c.r,c.w,c.alpha,diff);
    }
}
int main(void) {
    static uint16_t a[R_H][R_W], b[R_H][R_W];
    int cases = 0, bad = 0;
    srand(7);
    for (int i = 0; i < 400; i++) { test_case(i,a,b,&bad); cases++; }
    printf("%d/%d outline and box cases identical\n", cases - bad, cases);
    return bad != 0;
}
