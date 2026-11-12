// The integer upright bounds must equal the previous binary32 formula exactly.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/render_raster.c"

static uint32_t seed=73;
static int sample(int range) {
    seed=seed*1664525u+1013904223u;
    return (int)(seed%(unsigned)range);
}
static void old_bounds(const scene_t *s,prim_t *p,float extent) {
    if(extent==-2.f)return;
    float soft=p->soft/(float)Q,cx=p->cx/(float)Q,cy=p->cy/(float)Q;
    float hx=extent<0?p->a/(float)Q:ui_canvasf(extent);
    float hy=extent<0?p->b/(float)Q:ui_canvasf(extent);
    int x0=(int)floorf(cx-hx-soft)-1,x1=(int)ceilf(cx+hx+soft)+1;
    int y0=(int)floorf(cy-hy-soft)-1,y1=(int)ceilf(cy+hy+soft)+1;
    if(p->clip>=0) {
        const prim_t *c=&s->p[p->clip];
        x0=imax(x0,c->bx0);x1=imin(x1,c->bx1);
        y0=imax(y0,c->by0);y1=imin(y1,c->by1);
    }
    p->bx0=(int16_t)imax(x0,0);p->bx1=(int16_t)imin(x1,R_W-1);
    p->by0=(int16_t)imax(y0,0);p->by1=(int16_t)imin(y1,R_H-1);
}
int main(int argc,char **argv) {
    (void)argv;
    static scene_t scene;
    if(argc>1) {
        scene.p[0]=(prim_t){.cx=INT32_MAX,.a=16,.b=16,.soft=16,.clip=-1};
        s_extent[0]=-1.f;compute_bbox(&scene,&scene.p[0]);
        return 0;
    }
    for(unsigned i=0;i<200000;i++) {
        prim_t *p=&scene.p[0];memset(p,0,sizeof *p);
        p->cx=sample(R_W*Q*8)-R_W*Q*4;p->cy=sample(R_H*Q*8)-R_H*Q*4;
        p->a=sample(R_W*Q*2);p->b=sample(R_H*Q*2);p->soft=(int16_t)(1+sample(32*Q));
        p->clip=i%3? -1:1;
        scene.p[1]=(prim_t){.bx0=7,.bx1=R_W-11,.by0=5,.by1=R_H-13};
        p->bx0=11;p->bx1=19;p->by0=13;p->by1=23;
        float extent=i%17==0? -2.f:i%7==0?sample(1000)/7.f:-1.f;
        s_extent[0]=extent;prim_t expected=*p;old_bounds(&scene,&expected,extent);compute_bbox(&scene,p);
        assert(p->bx0==expected.bx0 && p->bx1==expected.bx1);
        assert(p->by0==expected.by0 && p->by1==expected.by1);
    }
    puts("bounds: 200000 binary32 oracle cases, signed Q4/fractional origins, clips, offscreen, radius and preset paths exact");
}
