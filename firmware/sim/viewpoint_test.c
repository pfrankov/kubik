#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../main/viewpoint.h"
static void step(viewpoint_t *v,float x,float y,float z,int count,int still) {
    float w[]={x,y,z};
    for(int i=0;i<count;i++) {viewpoint_step(v,w,still,.02f);assert(isfinite(v->x)&&isfinite(v->y));}
}
int main(void) {
    for(int standing=0;standing<2;standing++) {
        viewpoint_t v;viewpoint_init(&v);
        if(standing) { step(&v,1.570796f,0,0,50,0);step(&v,0,0,0,1500,1); }
        assert(fabsf(v.x)<.01f&&fabsf(v.y)<.01f);
        step(&v,0,-1,0,1,0);assert(v.x>.025f); // response within one sample
        assert(v.p[2]<v.q[2]); // the drawn pose runs ahead of the measured one
        step(&v,0,-1,0,9,0);float held=v.x;
        step(&v,0,0,0,100,1);assert(fabsf(v.x-held)<.001f); // dwell before adapting: 2 s held in the room
        step(&v,0,0,0,1200,1);assert(fabsf(v.x)<.01f); // new pose becomes neutral
        step(&v,.5f,0,0,20,0);assert(v.y>.3f);
        step(&v,-.5f,0,0,20,0);assert(fabsf(v.y)<.01f);
        step(&v,0,.4f,0,1000,0); // bounded even through large turns
        assert(fabsf(v.x)<=1&&fabsf(v.y)<=1);
    }
    puts("Adaptive viewpoint: lying/standing, immediate gyro response, stable dwell, gradual neutral and bounded turns passed");
}
