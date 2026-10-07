// The host-side synth cost probe, kept separate so a slow buffer can be replayed diagnostically.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "tess_sound_probe.h"

enum { COST_REPLAYS = 5 };

static double elapsed_us(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_nsec - start.tv_nsec) / 1e3;
}

static uint64_t frame_signature(const int32_t *frame) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < PROBE_FRAME; i++) {
        uint32_t sample = (uint32_t)frame[i];
        for (int byte = 0; byte < 4; byte++) {
            hash ^= (sample >> (byte * 8)) & 0xff;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static void start_cost_play(tess_sound_t *sound, int play) {
    tess_sound_seed(sound, 77 + play);
    probe_rest(sound);
    if (play % 2) tess_sound_cue(sound, TC_LOVE, .5f, 0);
    else tess_sound_event(sound, SFX_SETUP_OK, -1, 0, .5f);
}

static bool reconstruct_cost_frame(tess_sound_t *sound, const tess_sound_t *initial, int target_play,
                                   int target_frame, uint64_t expected, tess_sound_t *target_state) {
    int32_t frame[PROBE_FRAME];
    *sound = *initial;
    tess_sound_reset(sound);
    probe_rest(sound);
    for (int play = 0; play <= target_play; play++) {
        start_cost_play(sound, play);
        for (int frame_no = 0; tess_sound_active(sound); frame_no++) {
            memset(frame, 0, sizeof frame);
            if (play == target_play && frame_no == target_frame) *target_state = *sound;
            tess_sound_mix(sound, frame, PROBE_FRAME);
            if (play == target_play && frame_no == target_frame) return frame_signature(frame) == expected;
        }
    }
    return false;
}

static void replay_cost_frame(tess_sound_t *sound, const tess_sound_t *target_state, uint64_t expected) {
    int32_t frame[PROBE_FRAME];
    for (int replay = 1; replay <= COST_REPLAYS; replay++) {
        *sound = *target_state;
        memset(frame, 0, sizeof frame);
        struct timespec wall_start, cpu_start, cpu_end, wall_end;
        assert(clock_gettime(CLOCK_MONOTONIC, &wall_start) == 0);
        assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start) == 0);
        tess_sound_mix(sound, frame, PROBE_FRAME);
        assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0);
        assert(clock_gettime(CLOCK_MONOTONIC, &wall_end) == 0);
        double cpu_us = elapsed_us(cpu_start, cpu_end);
        double wall_us = elapsed_us(wall_start, wall_end);
        uint64_t signature = frame_signature(frame);
        fprintf(stderr, "synth: replay %d/%d CPU %.1f us, monotonic %.1f us, PCM %s\n",
                replay, COST_REPLAYS, cpu_us, wall_us, signature == expected ? "match" : "MISMATCH");
    }
}

void check_cost(tess_sound_t *sound) {
    // Keep one pristine state for failure-only reconstruction; don't perturb the primary timed loop.
    tess_sound_t initial = *sound;
    int32_t frame[PROBE_FRAME];
    tess_sound_reset(sound);
    probe_rest(sound);
    double worst = 0, total = 0, worst_wall = 0;
    long buffers = 0;
    int worst_play = -1, worst_frame = -1, worst_seed = 0;
    const char *worst_sound = "none";
    int worst_voices = 0, worst_pending = 0;
    unsigned worst_reverb = 0;
    uint64_t worst_signature = 0;
    for (int play = 0; play < 40; play++) {
        start_cost_play(sound, play);
        const char *sound_name = play % 2 ? "TC_LOVE" : "SFX_SETUP_OK";
        int frame_no = 0;
        while (tess_sound_active(sound)) {
            memset(frame, 0, sizeof frame);
            struct timespec wall_start, cpu_start, cpu_end, wall_end;
            assert(clock_gettime(CLOCK_MONOTONIC, &wall_start) == 0);
            assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start) == 0);
            tess_sound_mix(sound, frame, PROBE_FRAME);
            assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0);
            assert(clock_gettime(CLOCK_MONOTONIC, &wall_end) == 0);
            double cpu_us = elapsed_us(cpu_start, cpu_end);
            double wall_us = elapsed_us(wall_start, wall_end);
            if (cpu_us > worst) {
                worst = cpu_us;
                worst_wall = wall_us;
                worst_play = play;
                worst_frame = frame_no;
                worst_seed = 77 + play;
                worst_sound = sound_name;
                worst_voices = sound->active_voices;
                worst_pending = sound->pending_count - sound->pending_head;
                worst_reverb = sound->reverb_samples_left;
                worst_signature = frame_signature(frame);
            }
            total += cpu_us;
            buffers++;
            frame_no++;
        }
    }
    fprintf(stderr, "synth: %.1f us CPU per 20 ms buffer on average, %.1f us at worst (host thread)\n",
            total / buffers, worst);
    fprintf(stderr, "synth: worst input play %d seed %d %s frame %d: CPU %.1f us, monotonic %.1f us; "
                    "after mix: %d voices, %d pending, %u reverb samples, PCM %016" PRIx64 "\n",
            worst_play, worst_seed, worst_sound, worst_frame, worst, worst_wall,
            worst_voices, worst_pending, worst_reverb, worst_signature);
    if (worst >= 1000) {
        tess_sound_t target_state;
        bool matches = reconstruct_cost_frame(sound, &initial, worst_play, worst_frame, worst_signature, &target_state);
        fprintf(stderr, "synth: deterministic worst-frame reconstruction PCM %s\n", matches ? "match" : "MISMATCH");
        if (matches) replay_cost_frame(sound, &target_state, worst_signature);
    }
    assert(worst < 1000);  // 5 % of the buffer's time, on a host that is tens of times faster than the device
}
