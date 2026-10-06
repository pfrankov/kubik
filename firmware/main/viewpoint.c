#include "viewpoint.h"
#include <math.h>
#include <string.h>
#define VIEW_LEAD 0.045f  // s: IMU sample to photons (sample period, frame, transfer)
#define REST_S 2.5f       // stillness before the resting pose starts to become the new neutral
#define RELAX_S 4.f       // ... and the time constant of that drift
static float clamp(float x,float lo,float hi) { return fminf(hi,fmaxf(lo,x)); }
void viewpoint_init(viewpoint_t *v) { memset(v,0,sizeof *v);v->q[0]=v->p[0]=1; }
void viewpoint_step(viewpoint_t *v,const float w[3],bool still,float dt) {
    dt=clamp(dt,0,.1f);
    float a=v->q[0],b=v->q[1],c=v->q[2],d=v->q[3],h=dt*.5f;
    v->q[0]+=(-b*w[0]-c*w[1]-d*w[2])*h;
    v->q[1]+=(a*w[0]+c*w[2]-d*w[1])*h;
    v->q[2]+=(a*w[1]+d*w[0]-b*w[2])*h;
    v->q[3]+=(a*w[2]+b*w[1]-c*w[0])*h;
    v->quiet=still?fminf(60,v->quiet+dt):0;
    // Moving the neutral orientation is equivalent to relaxing the relative quaternion towards identity.
    // Only after a real rest, and slowly: while the cube is in the hand the object stays put in the room.
    if(v->q[0]<0) for(int i=0;i<4;i++) v->q[i]=-v->q[i];
    if(v->quiet>REST_S) {
        float k=dt/(RELAX_S+dt);
        v->q[0]+=(1-v->q[0])*k;
        for(int i=1;i<4;i++) v->q[i]*=1-k;
    }
    float norm=sqrtf(v->q[0]*v->q[0]+v->q[1]*v->q[1]+v->q[2]*v->q[2]+v->q[3]*v->q[3]);
    for(int i=0;i<4;i++) v->q[i]/=norm;
    // Predicted pose: the same integration step over the latency.
    a=v->q[0];b=v->q[1];c=v->q[2];d=v->q[3];h=VIEW_LEAD*.5f;
    v->p[0]=a+(-b*w[0]-c*w[1]-d*w[2])*h;
    v->p[1]=b+(a*w[0]+c*w[2]-d*w[1])*h;
    v->p[2]=c+(a*w[1]+d*w[0]-b*w[2])*h;
    v->p[3]=d+(a*w[2]+b*w[1]-c*w[0])*h;
    norm=sqrtf(v->p[0]*v->p[0]+v->p[1]*v->p[1]+v->p[2]*v->p[2]+v->p[3]*v->p[3]);
    for(int i=0;i<4;i++) v->p[i]/=norm;
    float n=sqrtf(v->q[1]*v->q[1]+v->q[2]*v->q[2]+v->q[3]*v->q[3]);
    float k=n>1e-6f?2*atan2f(n,v->q[0])/n:2;
    v->x=clamp(-v->q[2]*k/.55f,-1,1);
    v->y=clamp(v->q[1]*k/.55f,-1,1);
}
