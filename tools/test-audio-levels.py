#!/usr/bin/env python3
"""Shared audible taper, independent channels, exact mute and unchanged peak safety."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
audio = (ROOT / 'firmware/main/audio.c').read_text()
mixer = audio[audio.index('static bool mix_speaker_frame('):audio.index('static void speaker_cycle(')]
TEST = r'''
#include <assert.h>
#include <math.h>
#include "audio_levels.h"
#include "audio_signal.h"
#include <string.h>
#define SPK_FRAME 8
static float s_spk_level;
static bool s_speech_audible;
static int fx;
static bool audible;
static void audio_sfx_note_output(bool value) { audible=value; }
static void audio_sfx_mix(int32_t *mix, int n) { for (int i=0;i<n;i++) mix[i]+=fx; }
@MIXER@
static void mixed_sources(void) {
    int16_t samples[SPK_FRAME]; int32_t mix[SPK_FRAME];
    float history[4]={0}; int at=0;
    audio_levels_set_speech(100); audio_levels_set_interface(100);
    for (int i=0;i<SPK_FRAME;i++) mix[i]=10000;
    fx=0; assert(mix_speaker_frame(samples,mix,true,history,&at));
    float voice=history[0]; assert(samples[0]==10000);
    for (int i=0;i<SPK_FRAME;i++) mix[i]=10000;
    fx=20000; mix_speaker_frame(samples,mix,true,history,&at);
    assert(history[1]==voice && audible && samples[0]<32767 && samples[0]>20000);
    assert(audio_levels_mix(audio_levels_speech(32767)+1)==audio_levels_speech(32767)+1);
    for (int sign=-1;sign<=1;sign+=2) {
        fx=sign*3*32767;
        for (int i=0;i<SPK_FRAME;i++) mix[i]=sign*32767;
        mix_speaker_frame(samples,mix,true,history,&at);
        assert(sign*samples[0]>32000 && sign*samples[0]<32767);
    }
    fx=20000;
    audio_levels_set_speech(0);
    for (int i=0;i<SPK_FRAME;i++) mix[i]=10000;
    int muted_at=at;
    mix_speaker_frame(samples,mix,true,history,&at);
    assert(!s_speech_audible && history[muted_at]==0 && samples[0]>0);
    audio_levels_set_interface(0);
    for (int i=0;i<SPK_FRAME;i++) mix[i]=10000;
    assert(!mix_speaker_frame(samples,mix,true,history,&at)); assert(samples[0]==0 && !audible);
}

static int16_t reference_limit(int32_t value, int32_t knee) {
    int64_t magnitude = value < 0 ? -(int64_t)value : value;
    if (magnitude > knee) { int64_t excess=magnitude-knee,h=32767-knee; magnitude=knee+h*excess/(h+excess); }
    return (int16_t)(value < 0 ? -magnitude : magnitude);
}
static void exact_levels(void) {
    uint32_t seed=19;
    for(int i=0;i<1000000;i++) {
        seed=seed*1664525u+1013904223u; int32_t value; memcpy(&value,&seed,sizeof value);
        assert(audio_levels_limit(value)==reference_limit(value,24576));
        assert(audio_levels_mix(value)==reference_limit(value,32000));
    }
    int last_gain=-1;
    for(int p=0;p<=100;p++) {
        audio_levels_set_speech(p);
        audio_levels_set_interface(p);
        int32_t gain=audio_levels_interface(65536);
        int32_t expected=(int32_t)lround(pow(p/100.,1.5)*65536.);
        assert(abs(gain-expected)<=1); // independent double-precision oracle, Q16 rounding
        assert(gain>last_gain && gain<=65536);
        assert(audio_levels_interface(-65536)==-gain);
        assert(audio_levels_speech_enabled()==(p>0));
        assert(audio_levels_interface_enabled()==(p>0));
        last_gain=gain;
        for(int sample=-32768;sample<=32767;sample++)
            assert(audio_levels_speech(sample)==reference_limit((int32_t)((int64_t)sample*gain/65536),24576));
    }
    // Audible lower/midrange; ceilings and limiter semantics above remain exact.
    const int levels[]={0,10,25,50,75,100}, amplitudes[]={0,316,1250,3535,6495,10000};
    for(unsigned i=0;i<sizeof levels/sizeof *levels;i++) {
        audio_levels_set_speech(levels[i]); audio_levels_set_interface(levels[i]);
        assert(abs(audio_levels_speech(10000)-amplitudes[i])<=1);
        assert(audio_levels_interface(10000)==audio_levels_speech(10000));
    }
}

int main(void) {
    exact_levels();
    mixed_sources();
    audio_levels_set_interface(70);
    int ui = audio_levels_interface(10000);
    assert(abs(ui - 5857) <= 1);
    audio_levels_set_speech(70);
    assert(audio_levels_speech(10000) == ui); // both channels share the taper, never an extra boost
    audio_levels_set_speech(0);
    assert(!audio_levels_speech_enabled() && audio_levels_interface_enabled());
    assert(audio_levels_speech(32767) == 0 && audio_levels_interface(10000) == ui);
    audio_levels_set_speech(100); audio_levels_set_interface(0);
    assert(!audio_levels_interface_enabled() && audio_levels_speech_enabled());
    assert(audio_levels_interface(10000) == 0 && audio_levels_speech(10000) == 10000);
    assert(audio_levels_limit(32767 + 98301) < 32767);
    int last = 0;
    for (int p = 0; p <= 100; p++) {
        audio_levels_set_speech(p);
        int value = audio_levels_speech(4000);
        assert(value >= last && value <= 4000); last = value;
    }
    audio_levels_set_speech(100);
    last = 0;
    for (int sample = 0; sample <= 32767; sample++) {
        int value = audio_levels_speech(sample);
        assert(value >= last && value <= sample && audio_levels_speech(-sample) == -value);
        last = value;
    }
    assert(audio_levels_speech(-32768) > -32768);
    assert(audio_levels_limit(INT32_MIN)>-32768 && audio_levels_mix(INT32_MAX)<32767);
    audio_levels_set_speech(-1); assert(audio_levels_speech(10000) == 0);
    audio_levels_set_interface(101); assert(audio_levels_interface(10000) == 10000);
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-levels-') as directory:
    path = Path(directory); (path / 'test.c').write_text('#include <stdlib.h>\n' + TEST.replace('@MIXER@', mixer))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-Ifirmware/main', str(path / 'test.c'), 'firmware/main/audio_levels.c', 'firmware/main/audio_signal.c', '-lm', '-o', str(path / 'test')], cwd=ROOT, check=True)
    subprocess.run([str(path / 'test')], check=True)
print('audio levels: shared 0..100 taper, audible midpoint, independent mute, unity ceilings and bounded peaks passed')
