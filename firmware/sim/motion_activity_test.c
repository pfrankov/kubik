#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "motion_activity.h"

static void filtered_motion_settles(void) {
    motion_activity_t settling={0}; float gravity[3]={0,0,1};
    assert(!motion_activity_step(&settling,gravity,0,0,0));
    int events=0; int64_t last=0;
    for (int t=20;t<60000;t+=20) {
        gravity[0]+=(.5f-gravity[0])*.04f;
        gravity[2]+=(.8660254f-gravity[2])*.04f; // production filter settling after a hand stops
        if (motion_activity_step(&settling,gravity,0,0,t)) { events++; last=t; }
    }
    assert(events>0 && events<12 && last<5000); // finite tail, far shorter than the 45 s dim timer
}
int main(void) {
    motion_activity_t s = {0};
    float gravity[3] = {0,0,1};
    assert(!motion_activity_step(&s,gravity,0,0,0));
    // Small sensor noise and a stationary pose never refresh the idle timer, even over several minutes.
    for (int t=20;t<180000;t+=20) {
        gravity[0]=.005f*sinf(t*.01f); gravity[1]=.005f*cosf(t*.01f);
        assert(!motion_activity_step(&s,gravity,.02f,.01f,t));
    }
    int events=0; int64_t last=0;
    for (int t=180000;t<185000;t+=20) {
        float angle=(t-180000)*.00015f; // slow tilt: 8.6 degrees/s
        gravity[0]=sinf(angle); gravity[1]=0; gravity[2]=cosf(angle);
        if (motion_activity_step(&s,gravity,.15f,0,t)) {
            assert(!last || t-last>=MOTION_ACTIVITY_MS); last=t; events++;
        }
    }
    assert(events>=15 && events<=20);
    // Holding the tilted position still must allow normal idle/dim again.
    for (int t=185000;t<245000;t+=20) assert(!motion_activity_step(&s,gravity,0,0,t));
    // Much slower tilts accumulate pose change instead of depending on a gyro threshold.
    motion_activity_t slow={0}; events=0;
    for (int t=0;t<10000;t+=20) {
        float angle=t*.00002f;
        gravity[0]=sinf(angle); gravity[2]=cosf(angle);
        events+=motion_activity_step(&slow,gravity,.02f,0,t);
    }
    assert(events>=3 && events<=5);
    // Rotation about gravity and a straight lift have no gravity-direction change.
    assert(motion_activity_step(&slow,gravity,.2f,0,11000));
    assert(motion_activity_step(&slow,gravity,0,.08f,12000));
    for (int t=12001;t<12250;t++) assert(!motion_activity_step(&slow,gravity,2,2,t));
    filtered_motion_settles();
    motion_activity_t wrap={0};
    assert(!motion_activity_step(&wrap,gravity,.5f,0,UINT32_MAX-100));
    assert(!motion_activity_step(&wrap,gravity,.5f,0,100));
    assert(motion_activity_step(&wrap,gravity,.5f,0,200)); // unsigned interval spans timer wrap
    puts("motion activity: slow/fast tilt, lift, static tilt, sensor noise and bounded cadence passed");
}
