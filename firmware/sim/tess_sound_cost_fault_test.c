// Deterministic faults exercise the actual cost probe, including its failure exits.
// Only this host executable substitutes time; the mixer and replay path stay real.
#include <stdio.h>
#include <time.h>
#include "tess_sound_probe.h"

typedef struct {
    int initial_spikes, persistent_frame, persistent_us;
    uint32_t slow_replays;
    bool corrupt_pcm, enforce;
    int expected_replays;
} fault_case_t;

static const fault_case_t cases[] = {
    {0, -1, 0, 0, false, true, 0},               // ordinary cost
    {1, -1, 0, 0, false, true, 5},               // isolated first-pass spike
    {4, -1, 0, 0, false, true, 20},              // four spikes: all must be checked
    {5, -1, 0, 0, false, true, 0},               // five spikes: fail without replays
    {1, 4, 1500, UINT32_MAX, false, true, 5},     // persistent regression
    {1, 4, 1000, UINT32_MAX, false, true, 5},     // equality is over budget
    {1, 4, 999, UINT32_MAX, false, true, 5},      // just below the budget
    {1, 4, 1500, 0x09, false, true, 5},          // two slow replays: median passes
    {1, 4, 1500, 0x15, false, true, 5},          // three slow replays: median fails
    {2, 5, 1500, UINT32_MAX, false, true, 10},    // regression is not the slowest first buffer
    {1, -1, 0, 0, true, true, 5},                // changed PCM must fail
    {5, 4, 1500, UINT32_MAX, false, false, 20},   // instrumented probe ignores timing only
    {1, -1, 0, 0, true, false, 5},               // instrumented probe still checks PCM
};

static const fault_case_t *s_case;
static int s_play, s_frame, s_pass = -1;
static uint64_t s_cpu_ns;

static void fault_seed(tess_sound_t *sound, uint32_t seed) {
    tess_sound_seed(sound, seed);
    s_play = (int)seed - 77;
    s_frame = 0;
    if (s_play == 0) s_pass++;
}

static int injected_cost_us(void) {
    if (s_play != 0) return 20;
    if (s_pass == 0 && s_frame >= 4 && s_frame < 4 + s_case->initial_spikes)
        return 2000 - (s_frame - 4) * 100;
    bool slow = s_pass == 0 || (s_case->slow_replays & (UINT32_C(1) << (s_pass - 1)));
    return s_frame == s_case->persistent_frame && slow ? s_case->persistent_us : 20;
}

static void fault_mix(tess_sound_t *sound, int32_t *frame, int count) {
    tess_sound_mix(sound, frame, count);
    s_cpu_ns += (uint64_t)injected_cost_us() * 1000;
    if (s_case->corrupt_pcm && s_pass > 0 && s_play == 0 && s_frame == 4) frame[0] ^= 1;
    s_frame++;
}

static int fault_clock(clockid_t clock_id, struct timespec *out) {
    if (clock_id != CLOCK_THREAD_CPUTIME_ID) return clock_gettime(clock_id, out);
    out->tv_sec = (time_t)(s_cpu_ns / UINT64_C(1000000000));
    out->tv_nsec = (long)(s_cpu_ns % UINT64_C(1000000000));
    return 0;
}

// The header above defines rest/silence using the real mixer. Intercept only the
// measured calls and per-play seeds in the probe, never the production synth.
#define clock_gettime fault_clock
#define tess_sound_seed fault_seed
#define tess_sound_mix fault_mix
#include "tess_sound_cost_test.c"
#undef tess_sound_mix
#undef tess_sound_seed
#undef clock_gettime

int main(int argc, char **argv) {
    assert(argc == 2);
    int index = atoi(argv[1]);
    assert(index >= 0 && index < (int)(sizeof cases / sizeof cases[0]));
    s_case = &cases[index];
    static tess_sound_t sound;
    check_cost(&sound, s_case->enforce);
    assert(s_pass == s_case->expected_replays);
    return 0;
}
