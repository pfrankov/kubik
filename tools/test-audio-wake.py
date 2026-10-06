#!/usr/bin/env python3
"""Actual Tess-event and PCM publishers wake only after releasing their queue lock."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sfx = (ROOT / "firmware/main/audio_sfx.c").read_text()
tess = sfx[sfx.index("static void tess_event"):sfx.index("void audio_tess_cue")]
cue = sfx[sfx.index("void audio_tess_cue"):sfx.index("static void play_pcm")]
pcm = sfx[sfx.index("static void play_pcm"):sfx.index("void audio_sfx(sfx_t")]
voices = sfx[sfx.index("#define MAX_VOICES"):sfx.index("static void load_kit")]
mock = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio.h"
#include "audio_levels.h"
#define portMAX_DELAY 1
static int s_sfx_mtx = 1, locked;
static character_t s_character;
static void *s_tess;
static float s_tess_position, s_tess_energy;
static unsigned queued, kicks;
static bool mic_open, speaking;
bool audio_mic_is_open(void) { return mic_open; }
bool audio_stream_playing(void) { return speaking; }
static bool tess_sound_event_is_utterance(sfx_t id) { return id == SFX_GIGGLE; }
static bool tess_sound_cue_is_utterance(tess_cue_t id) { return id == TC_LOVE; }
static void xSemaphoreTake(int mutex, int timeout) { (void)mutex; (void)timeout; assert(!locked); locked = 1; }
static void xSemaphoreGive(int mutex) { (void)mutex; assert(locked); locked = 0; }
static void tess_sound_event(void *engine, sfx_t id, int index, float position, float energy) {
    (void)engine; (void)id; (void)index; (void)position; (void)energy; assert(locked); queued++;
}
static bool tess_sound_cue(void *engine, tess_cue_t id, float strength, float position) {
    (void)engine; (void)id; (void)strength; (void)position; assert(locked); queued++; return true;
}
'''
check = r'''
void audio_kick(void) {
    assert(!locked); // the higher-priority reader must never be woken under the writer's lock
    if (s_character == CHARACTER_TESS) assert(queued);
    else assert(s_voices[0].left > 0);
    kicks++;
}
int main(void) {
    s_character = CHARACTER_TESS; s_tess = &queued;
    tess_event(SFX_WAKE, -1); assert(kicks == 1 && queued == 1);
    tess_event(SFX_VOLUME, 2); assert(kicks == 2 && queued == 2);
    mic_open = true; tess_event(SFX_GIGGLE, -1); assert(kicks == 2 && queued == 2);
    mic_open = false; speaking = true; tess_event(SFX_GIGGLE, -1); assert(kicks == 2 && queued == 2);
    speaking = false;
    mic_open = true; audio_tess_cue(TC_LOVE, 1, 0); assert(kicks == 2 && queued == 2);
    mic_open = false; speaking = true; audio_tess_cue(TC_LOVE, 1, 0); assert(kicks == 2 && queued == 2);
    speaking = false;
    s_character = CHARACTER_PLUSH; static int16_t samples[4];
    play_pcm(samples, 4, 65536); assert(kicks == 3 && s_voices[0].left == 3);
    play_pcm(NULL, 0, 65536); assert(kicks == 3); // refusal doesn't spuriously wake output
    audio_levels_set_interface(0);
    tess_event(SFX_WAKE, -1); play_pcm(samples, 4, 65536);
    assert(kicks == 3 && queued == 2); // mute cannot suppress capture or wake the codec
    audio_levels_set_interface(70); tess_event(SFX_WAKE, -1);
    assert(kicks == 4 && queued == 3);
    puts("audio wake: Tess/stepped/PCM publication precedes unlocked speaker notification");
}
'''
with tempfile.TemporaryDirectory(prefix="kubik-audio-wake-") as tmp:
    source = Path(tmp) / "wake.c"
    source.write_text(mock + voices + tess + cue + pcm + check)
    exe = Path(tmp) / "wake"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "firmware/main"),
                    str(source), str(ROOT / "firmware/main/audio_levels.c"), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
