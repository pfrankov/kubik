#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auto_voice.h"
#include "wake_probability.h"
#include "wake_resample.h"
static void probability_test(void) {
    assert(!wake_should_trigger(99, 255*5)); // full confidence cannot bypass warmup
    assert(!wake_should_trigger(100, 73*5)); // measured ordinary speech
    assert(wake_should_trigger(100, 252*5)); // measured positive missed by the source 99% cutoff
    assert(!wake_should_trigger(100, 1241));
    assert(wake_should_trigger(100, 1242)); // exact 97% boundary under scale 1/256
}
static void policy_test(void) {
    auto_voice_t s = {0};
    for (int i = 0; i < 399; i++) assert(auto_voice_step(&s, false, false, 20) == AUTO_CONTINUE);
    assert(auto_voice_step(&s, false, false, 20) == AUTO_EMPTY);
    s = (auto_voice_t){.warmup_ms=160};
    for (int i = 0; i < 8; i++) assert(auto_voice_step(&s, true, false, 20) == AUTO_CONTINUE);
    assert(!s.heard); // keyword boundary is ignored
    for (int i = 0; i < 6; i++) assert(auto_voice_step(&s, true, false, 20) == AUTO_CONTINUE);
    assert(s.heard);
    for (int i = 0; i < 39; i++) assert(auto_voice_step(&s, false, false, 20) == AUTO_CONTINUE);
    assert(auto_voice_step(&s, false, false, 20) == AUTO_SEND);
    s = (auto_voice_t){.elapsed_ms=200, .heard=true, .quiet_ms=780};
    assert(auto_voice_step(&s, false, true, 20) == AUTO_CONTINUE && !s.quiet_ms);
    s = (auto_voice_t){.elapsed_ms=59980, .heard=true};
    assert(auto_voice_step(&s, true, false, 20) == AUTO_SEND);
    s = (auto_voice_t){.elapsed_ms=59980};
    assert(auto_voice_step(&s, false, true, 20) == AUTO_EMPTY);
}
static void resample_test(void) {
    int16_t input[960], whole[640], chunked[640];
    for (int i = 0; i < 960; i++) input[i] = (int16_t)(10000*sin(i*.22));
    wake_resample_t a={0},b={0};
    assert(wake_resample(&a,input,960,whole)==640);
    for(int i=0;i<4;i++) assert(wake_resample(&b,input+240*i,240,chunked+160*i)==160);
    assert(!memcmp(whole,chunked,sizeof whole));
    for(int i=0;i<960;i++)input[i]=10000;
    a=(wake_resample_t){0};wake_resample(&a,input,960,whole);
    for(int i=32;i<640;i++)assert(abs(whole[i]-10000)<=3);
    // Frequencies above the new Nyquist limit must be attenuated, not aliased at full amplitude.
    for(int i=0;i<960;i++)input[i]=(int16_t)(10000*sin(2*3.141592653589793*10000*i/24000));
    a=(wake_resample_t){0};wake_resample(&a,input,960,whole);
    double energy=0;for(int i=32;i<640;i++)energy+=(double)whole[i]*whole[i];
    assert(sqrt(energy/608)<500);
}
int main(void) { probability_test();policy_test();resample_test();puts("automatic voice deadlines and streaming resampler ok"); }
