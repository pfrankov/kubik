// The closed-form outline path (raster_frame) must match the generic sphere tracer. Pixels whose distance sits
// within integer-sqrt rounding of a coverage step may differ by a couple of levels; nothing else may.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/render_internal.h"

static uint16_t img[R_H][R_W];
static void push(int x0, int y0, int x1, int y1, uint16_t *px, void *ctx) {
    for (int y = y0; y < y1; y++) memcpy(&img[y][x0], px + (y - y0) * (x1 - x0), (size_t)(x1 - x0) * 2);
}
static render_workspace_t workspace;
static void draw(float cx, float cy, float hw, float hh, float r, float w, float alpha, bool generic, uint16_t out[R_H][R_W]) {
    // w = 0: a plain box (sharp ones take raster_rect)
    static scene_t s;
    render_state_t rs = {0};
    scene_begin(&s, 0x0841);
    sc_rbox(&s, 240, 240, 200, 120, 30, 0.3f, 0x3366ff, 1);  // something underneath for blending
    prim_t *p = sc_rbox(&s, cx, cy, hw, hh, r, 0, 0xff2020, alpha);
    if (w > 0) pr_stroke(p, w);
    if (generic) {  // a cut that never cuts forces the generic tracer; distances are unchanged
        p->ncut = 1; p->cut_nx[0] = 16384; p->cut_ny[0] = 0; p->cut_c[0] = 1 << 24;
    }
    render_frame(&rs, &s, &workspace, false, push, NULL, NULL);
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
// Independent direct full-band raster: remove only the derived flags for the oracle.
static void raster_flags_frame(scene_t *s, bool generic, bool swap, uint16_t out[R_H][R_W]) {
    static uint16_t band[R_W*R_BAND];
    render_raster_prepare(s,swap);
    if(generic)for(int i=0;i<s->n;i++)s->p[i].raster_flags=0;
    for(int y=0;y<R_H;y+=R_BAND) {
        render_raster_window(s,band,0,R_W-1,y,y+R_BAND-1);
        memcpy(out[y],band,sizeof band);
    }
}
static void compare_flags(scene_t *s,bool swap) {
    static scene_t reference;
    static uint16_t optimized[R_H][R_W],generic[R_H][R_W];
    scene_clone(&reference,s);
    raster_flags_frame(s,false,swap,optimized);
    raster_flags_frame(&reference,true,swap,generic);
    assert(!memcmp(optimized,generic,sizeof optimized));
    scene_end(&reference);
}
static prim_t *flags_scene(scene_t *s,int clip_kind) {
    scene_begin(s,0x2945);
    prim_t *clip=NULL;
    if(clip_kind) {
        float half=clip_kind==1?140:clip_kind==2?70:40;
        float cy=clip_kind==3?205:220;
        clip=sc_mask_rbox(s,240,cy,half,clip_kind==1?110:45,15,0);
    }
    prim_t *box=sc_rbox(s,240,220,70,45,15,0,0xE61A7C,.73f);
    if(clip)pr_clip(s,box,clip);
    return box;
}
static void flags_reset(void) {
    static scene_t scene,cloned;
    for(int kind=0;kind<4;kind++)for(int swap=0;swap<2;swap++) {
        prim_t *box=flags_scene(&scene,kind);
        for(int i=0;i<scene.n;i++)scene.p[i].raster_flags=255;
        render_raster_prepare(&scene,swap);
        unsigned expected=kind==0?RASTER_PLAIN:kind==1?RASTER_NOCLIP|RASTER_PLAIN:0;
        assert(box->raster_flags==expected);
        compare_flags(&scene,swap);
        // Clone a prepared scene, then make its clip cease covering it. Reprepare must clear stale bits.
        scene_clone(&cloned,&scene);
        if(kind)cloned.p[0].a=cloned.p[0].b=16;
        else cloned.p[cloned.n-1].ncut=1;
        for(int i=0;i<cloned.n;i++)cloned.p[i].raster_flags=255;
        render_raster_prepare(&cloned,swap);
        assert(!cloned.p[cloned.n-1].raster_flags);
        compare_flags(&cloned,swap);
        scene_end(&cloned);scene_end(&scene);
    }
    puts("primitive flags: poison/reset, prepared clone, covering/equal/partial clips and byte order match flags-zero raster exactly");
}
static void straight_filled_rows(void) {
    static scene_t scene;
    // Compare against the old flags-zero sphere tracer, including both byte orders,
    // fractional origins/radii, thin boxes and rows at the straight/corner boundary.
    for (unsigned i=0;i<192;i++) for (int swap=0;swap<2;swap++) {
        scene_begin(&scene,0x2945);
        float cx=240+(int)(i%17)-8+i%3*.27f, cy=240+(int)(i%23)-11+i%5*.19f;
        float hw=2+(i*13)%153, hh=2+(i*17)%187;
        float radius=fminf(hw,hh)*(i%13)/12.f;
        prim_t *p=sc_rbox(&scene,cx,cy,hw,hh,radius,0,0xE61A7C,((i*73u)%256)/255.f);
        if(i%7==0)pr_soft(p,5); // Existing soft-step interpolation must stay untouched.
        compare_flags(&scene,swap);
        scene_end(&scene);
    }
    puts("filled rounded boxes: straight rows match the flags-zero scalar raster bit-for-bit");
}
int main(void) {
    static uint16_t a[R_H][R_W], b[R_H][R_W];
    int cases = 0, bad = 0;
    flags_reset();straight_filled_rows();
    srand(7);
    for (int i = 0; i < 400; i++) { test_case(i,a,b,&bad); cases++; }
    printf("%d/%d outline and box cases identical\n", cases - bad, cases);
    return bad != 0;
}
