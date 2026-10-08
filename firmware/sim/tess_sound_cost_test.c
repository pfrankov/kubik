// The host-side synth cost probe, kept separate so a slow buffer can be replayed diagnostically.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#include "tess_sound_probe.h"

enum { COST_REPLAYS = 5 };

static double elapsed_us(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_nsec - start.tv_nsec) / 1e3;
}

// Raw timestamps keep phase probes cheap; conversion happens outside the CPU interval.
#ifdef __APPLE__
static mach_timebase_info_data_t s_timebase;
static void phase_clock_init(void) { assert(mach_timebase_info(&s_timebase) == KERN_SUCCESS); }
static uint64_t phase_stamp(void) { return mach_absolute_time(); }
static double phase_us(uint64_t start, uint64_t end) {
    return (double)(end - start) * s_timebase.numer / s_timebase.denom / 1000;
}
#else
static void phase_clock_init(void) {}
static uint64_t phase_stamp(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &t) == 0);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static double phase_us(uint64_t start, uint64_t end) { return (double)(end - start) / 1000; }
#endif

typedef struct {
    double cpu_us, wall_us, mix_us, start_clock_us, end_clock_us;
} mix_cost_t;

static mix_cost_t measure_mix(tess_sound_t *sound, int32_t *frame) {
    struct timespec cpu_start, cpu_end;
    uint64_t wall_start = phase_stamp();
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start) == 0);
    uint64_t mix_start = phase_stamp();
    tess_sound_mix(sound, frame, PROBE_FRAME);
    uint64_t mix_end = phase_stamp();
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0);
    uint64_t wall_end = phase_stamp();
    return (mix_cost_t){
        .cpu_us = elapsed_us(cpu_start, cpu_end), .wall_us = phase_us(wall_start, wall_end),
        .mix_us = phase_us(mix_start, mix_end),
        .start_clock_us = phase_us(wall_start, mix_start),
        .end_clock_us = phase_us(mix_end, wall_end),
    };
}

static void report_phases(const char *label, mix_cost_t cost) {
    fprintf(stderr, "synth: %s raw monotonic phase envelopes: start CPU-clock read %.1f us, mix %.1f us, end CPU-clock read %.1f us\n",
            label, cost.start_clock_us, cost.mix_us, cost.end_clock_us);
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
        mix_cost_t cost = measure_mix(sound, frame);
        uint64_t signature = frame_signature(frame);
        fprintf(stderr, "synth: replay %d/%d CPU %.1f us, monotonic %.1f us, PCM %s\n",
                replay, COST_REPLAYS, cost.cpu_us, cost.wall_us, signature == expected ? "match" : "MISMATCH");
        report_phases("replay", cost);
    }
}

void check_cost(tess_sound_t *sound) {
    // Keep one pristine sound state for failure-only reconstruction.
    phase_clock_init();
    tess_sound_t initial = *sound;
    int32_t frame[PROBE_FRAME];
    tess_sound_reset(sound);
    probe_rest(sound);
    double worst = 0, total = 0;
    mix_cost_t worst_cost = {0};
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
            mix_cost_t cost = measure_mix(sound, frame);
            double cpu_us = cost.cpu_us;
            if (cpu_us > worst) {
                worst = cpu_us;
                worst_cost = cost;
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
            worst_play, worst_seed, worst_sound, worst_frame, worst, worst_cost.wall_us,
            worst_voices, worst_pending, worst_reverb, worst_signature);
    report_phases("primary worst buffer", worst_cost);
    if (worst >= 1000) {
        tess_sound_t target_state;
        bool matches = reconstruct_cost_frame(sound, &initial, worst_play, worst_frame, worst_signature, &target_state);
        fprintf(stderr, "synth: deterministic worst-frame reconstruction PCM %s\n", matches ? "match" : "MISMATCH");
        if (matches) replay_cost_frame(sound, &target_state, worst_signature);
    }
    assert(worst < 1000);  // 5 % of the buffer's time, on a host that is tens of times faster than the device
}
