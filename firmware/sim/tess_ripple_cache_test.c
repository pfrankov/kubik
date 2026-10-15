// Compare cached per-frame ripple terms with the prior ordered point transform.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"
#define RIPPLE_LIFE_S .9f
#define RIPPLE_SPEED 6.5f
#define RIPPLE_HALF .5f
#define RIPPLE_FADE .5f
#define RIPPLE_PUSH .3f
#define RIPPLE_LIFT .35f
void reference_ripple_point(const face_t *f, float *x, float *y, float *z) {
    for (int k = 0; k < TESS_RIPPLES; k++) {
        const tess_ripple_t *r = &f->tess_ripple[k];
        if (r->age >= RIPPLE_LIFE_S) continue;
        float dx = *x - r->x, dy = *y - r->y, d2 = dx * dx + dy * dy, ring = r->age * RIPPLE_SPEED;
        float near = fmaxf(0, ring - RIPPLE_HALF), far = ring + RIPPLE_HALF;
        if (d2 < near * near || d2 > far * far) continue;  // (not yet reached, or past)
        float d = sqrtf(d2) + 1e-3f, u = (d - ring) / RIPPLE_HALF;
        float gain = (1 - u * u) * (1 - r->age / RIPPLE_LIFE_S) / (1 + RIPPLE_FADE * d);
        *x += dx / d * RIPPLE_PUSH * gain;
        *y += dy / d * RIPPLE_PUSH * gain;
        *z += RIPPLE_LIFT * gain;
    }
}


static uint32_t rng=7;
static float value(void) { rng=rng*1664525u+1013904223u;return (rng>>8)*(1.f/16777216.f); }
int main(void) {
    face_t face={0};
    for(int frame=0;frame<1000;frame++) {
        for(int k=0;k<TESS_RIPPLES;k++)face.tess_ripple[k]=(tess_ripple_t){value()*4-2,value()*4-2,value()*2};
        tess_ripple_frame_t cache[TESS_RIPPLES];unsigned count=tess_ripple_frame(&face,cache);
        for(int i=0;i<112;i++) {
            float before[3]={value()*12-6,value()*12-6,value()*12-6},after[3];
            memcpy(after,before,sizeof after);
            reference_ripple_point(&face,&before[0],&before[1],&before[2]);
            tess_ripple_point(cache,count,&after[0],&after[1],&after[2]);
            assert(!memcmp(before,after,sizeof after));
        }
    }
    puts("ripples: 112000 ordered point transforms byte-exact with frame-cached bounds and fade");
}
