// The host-side synth cost probe, kept separate so a slow buffer can be replayed diagnostically.
#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "tess_sound_probe.h"

enum { COST_REPLAYS = 5 };

#if defined(__APPLE__) && defined(RUSAGE_INFO_V4)
#define HAVE_RUSAGE_INFO_V4 1
#endif

typedef struct {
    double probe_us;
    bool probe_time_valid;
#if defined(__APPLE__)
    bool process_valid, faults_valid;
    int process_errno, faults_errno;
#if defined(HAVE_RUSAGE_INFO_V4)
    struct rusage_info_v4 process;
#endif
    struct rusage faults;
#endif
} counter_sample_t;

typedef struct {
    unsigned samples, timed_samples, process_failures, fault_failures;
    int first_process_errno, first_fault_errno;
    double probe_total_us, probe_max_us;
    bool saw_instructions, saw_cycles;
} counter_stats_t;

static double elapsed_us(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_nsec - start.tv_nsec) / 1e3;
}

// Apple counters are process-wide. Probe overhead stays outside the existing per-mix CPU timing bracket.
static counter_sample_t read_counters(void) {
    counter_sample_t sample = {0};
#if defined(__APPLE__)
    struct timespec start, end;
    bool timed = clock_gettime(CLOCK_MONOTONIC, &start) == 0;
#if defined(HAVE_RUSAGE_INFO_V4)
    errno = 0;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&sample.process) == 0) sample.process_valid = true;
    else sample.process_errno = errno;
#endif
    errno = 0;
    if (getrusage(RUSAGE_SELF, &sample.faults) == 0) sample.faults_valid = true;
    else sample.faults_errno = errno;
    timed = timed && clock_gettime(CLOCK_MONOTONIC, &end) == 0;
    if (timed) {
        sample.probe_time_valid = true;
        sample.probe_us = elapsed_us(start, end);
    }
#endif
    return sample;
}

static void record_counter_sample(counter_stats_t *stats, const counter_sample_t *sample) {
#if defined(__APPLE__)
    stats->samples++;
    if (sample->probe_time_valid) {
        stats->timed_samples++;
        stats->probe_total_us += sample->probe_us;
        if (sample->probe_us > stats->probe_max_us) stats->probe_max_us = sample->probe_us;
    }
#if defined(HAVE_RUSAGE_INFO_V4)
    if (sample->process_valid) {
        stats->saw_instructions |= sample->process.ri_instructions != 0;
        stats->saw_cycles |= sample->process.ri_cycles != 0;
    } else {
        stats->process_failures++;
        if (!stats->first_process_errno) stats->first_process_errno = sample->process_errno;
    }
#endif
    if (!sample->faults_valid) {
        stats->fault_failures++;
        if (!stats->first_fault_errno) stats->first_fault_errno = sample->faults_errno;
    }
#else
    (void)stats;
    (void)sample;
#endif
}

#if defined(__APPLE__)
static void print_counter_error(const char *label, const char *api, int error) {
    fprintf(stderr, "synth: %s %s failed: errno %d", label, api, error);
    if (error) fprintf(stderr, " (%s)", strerror(error));
    fputc('\n', stderr);
}
#endif

#if defined(__APPLE__) && defined(HAVE_RUSAGE_INFO_V4)
static bool process_delta_is_monotonic(const counter_sample_t *before, const counter_sample_t *after) {
    return after->process.ri_instructions >= before->process.ri_instructions &&
           after->process.ri_cycles >= before->process.ri_cycles &&
           after->process.ri_user_time >= before->process.ri_user_time &&
           after->process.ri_system_time >= before->process.ri_system_time &&
           after->process.ri_pageins >= before->process.ri_pageins;
}
#endif

#if defined(__APPLE__)
static bool faults_delta_is_monotonic(const counter_sample_t *before, const counter_sample_t *after) {
    return after->faults.ru_minflt >= before->faults.ru_minflt &&
           after->faults.ru_majflt >= before->faults.ru_majflt &&
           after->faults.ru_nvcsw >= before->faults.ru_nvcsw &&
           after->faults.ru_nivcsw >= before->faults.ru_nivcsw;
}
#endif

static void print_counter_delta(const char *label, const counter_sample_t *before, const counter_sample_t *after) {
#if defined(__APPLE__)
    if (before->probe_time_valid && after->probe_time_valid)
        fprintf(stderr, "synth: %s counter snapshot overhead %.1f us\n", label, before->probe_us + after->probe_us);
    else fprintf(stderr, "synth: %s counter snapshot overhead unavailable (CLOCK_MONOTONIC probe failed)\n", label);
#if defined(HAVE_RUSAGE_INFO_V4)
    if (!before->process_valid) print_counter_error(label, "proc_pid_rusage before", before->process_errno);
    else if (!after->process_valid) print_counter_error(label, "proc_pid_rusage after", after->process_errno);
    else if (!process_delta_is_monotonic(before, after)) fprintf(stderr, "synth: %s process-counter delta unavailable: cumulative value decreased\n", label);
    else fprintf(stderr,
                 "synth: %s process delta: instructions=%" PRIu64 " cycles=%" PRIu64
                 " user_time=%" PRIu64 " system_time=%" PRIu64 " pageins=%" PRIu64 "\n",
                 label, after->process.ri_instructions - before->process.ri_instructions,
                 after->process.ri_cycles - before->process.ri_cycles,
                 after->process.ri_user_time - before->process.ri_user_time,
                 after->process.ri_system_time - before->process.ri_system_time,
                 after->process.ri_pageins - before->process.ri_pageins);
#else
    fprintf(stderr, "synth: %s process instruction/cycle counters unavailable: RUSAGE_INFO_V4 not exposed by this SDK\n", label);
#endif
    if (!before->faults_valid) print_counter_error(label, "getrusage before", before->faults_errno);
    else if (!after->faults_valid) print_counter_error(label, "getrusage after", after->faults_errno);
    else if (!faults_delta_is_monotonic(before, after)) fprintf(stderr, "synth: %s fault/context-switch delta unavailable: cumulative value decreased\n", label);
    else fprintf(stderr, "synth: %s getrusage delta: ru_minflt=%ld ru_majflt=%ld ru_nvcsw=%ld ru_nivcsw=%ld\n",
                 label, after->faults.ru_minflt - before->faults.ru_minflt,
                 after->faults.ru_majflt - before->faults.ru_majflt,
                 after->faults.ru_nvcsw - before->faults.ru_nvcsw,
                 after->faults.ru_nivcsw - before->faults.ru_nivcsw);
#else
    (void)label;
    (void)before;
    (void)after;
#endif
}

static void print_counter_summary(const counter_stats_t *stats) {
#if defined(__APPLE__)
    fprintf(stderr, "synth: counter snapshots %u (%u timed)", stats->samples, stats->timed_samples);
    if (stats->timed_samples)
        fprintf(stderr, ", probe wall avg %.1f us, max %.1f us", stats->probe_total_us / stats->timed_samples, stats->probe_max_us);
    else fputs(", probe wall unavailable", stderr);
    fputs("; process-wide deltas include probe/timing overhead and other threads\n", stderr);
    fputs("synth: counter sampling can perturb cache/frequency/duty context; deltas show correlation, not cause\n", stderr);
#if defined(HAVE_RUSAGE_INFO_V4)
    fprintf(stderr, "synth: proc_pid_rusage errors %u; instruction values %s, cycle values %s\n",
            stats->process_failures,
            stats->saw_instructions ? "observed nonzero" : "only zero observed (unsupported vs no increments is ambiguous)",
            stats->saw_cycles ? "observed nonzero" : "only zero observed (unsupported vs no increments is ambiguous)");
    if (stats->process_failures) print_counter_error("counter summary", "proc_pid_rusage first error", stats->first_process_errno);
#else
    fprintf(stderr, "synth: proc_pid_rusage RUSAGE_INFO_V4 not exposed by this SDK\n");
#endif
    fprintf(stderr, "synth: getrusage errors %u\n", stats->fault_failures);
    if (stats->fault_failures) print_counter_error("counter summary", "getrusage first error", stats->first_fault_errno);
#else
    (void)stats;
    fprintf(stderr, "synth: Apple process counters unavailable on this non-Apple build\n");
#endif
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

static void replay_cost_frame(tess_sound_t *sound, const tess_sound_t *target_state, uint64_t expected,
                              counter_stats_t *counter_stats) {
    int32_t frame[PROBE_FRAME];
    for (int replay = 1; replay <= COST_REPLAYS; replay++) {
        *sound = *target_state;
        memset(frame, 0, sizeof frame);
        counter_sample_t counters_before = read_counters();
        record_counter_sample(counter_stats, &counters_before);
        struct timespec wall_start, cpu_start, cpu_end, wall_end;
        assert(clock_gettime(CLOCK_MONOTONIC, &wall_start) == 0);
        assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start) == 0);
        tess_sound_mix(sound, frame, PROBE_FRAME);
        assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0);
        assert(clock_gettime(CLOCK_MONOTONIC, &wall_end) == 0);
        double cpu_us = elapsed_us(cpu_start, cpu_end);
        double wall_us = elapsed_us(wall_start, wall_end);
        counter_sample_t counters_after = read_counters();
        record_counter_sample(counter_stats, &counters_after);
        uint64_t signature = frame_signature(frame);
        fprintf(stderr, "synth: replay %d/%d CPU %.1f us, monotonic %.1f us, PCM %s\n",
                replay, COST_REPLAYS, cpu_us, wall_us, signature == expected ? "match" : "MISMATCH");
        char label[32];
        snprintf(label, sizeof label, "replay %d/%d", replay, COST_REPLAYS);
        print_counter_delta(label, &counters_before, &counters_after);
    }
}

void check_cost(tess_sound_t *sound) {
    // Keep one pristine state for failure-only reconstruction.
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
    counter_sample_t worst_counters_before = {0}, worst_counters_after = {0};
    bool worst_counters_captured = false;
    counter_stats_t counter_stats = {0};
    for (int play = 0; play < 40; play++) {
        start_cost_play(sound, play);
        const char *sound_name = play % 2 ? "TC_LOVE" : "SFX_SETUP_OK";
        int frame_no = 0;
        while (tess_sound_active(sound)) {
            memset(frame, 0, sizeof frame);
            counter_sample_t counters_before = read_counters();
            record_counter_sample(&counter_stats, &counters_before);
            struct timespec wall_start, cpu_start, cpu_end, wall_end;
            assert(clock_gettime(CLOCK_MONOTONIC, &wall_start) == 0);
            assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start) == 0);
            tess_sound_mix(sound, frame, PROBE_FRAME);
            assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end) == 0);
            assert(clock_gettime(CLOCK_MONOTONIC, &wall_end) == 0);
            double cpu_us = elapsed_us(cpu_start, cpu_end);
            double wall_us = elapsed_us(wall_start, wall_end);
            if (cpu_us > worst) {
                worst_counters_before = counters_before;
                worst_counters_after = read_counters();
                record_counter_sample(&counter_stats, &worst_counters_after);
                worst_counters_captured = true;
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
    if (worst_counters_captured) print_counter_delta("primary worst frame", &worst_counters_before, &worst_counters_after);
    if (worst >= 1000) {
        tess_sound_t target_state;
        bool matches = reconstruct_cost_frame(sound, &initial, worst_play, worst_frame, worst_signature, &target_state);
        fprintf(stderr, "synth: deterministic worst-frame reconstruction PCM %s\n", matches ? "match" : "MISMATCH");
        if (matches) replay_cost_frame(sound, &target_state, worst_signature, &counter_stats);
    }
    print_counter_summary(&counter_stats);
    assert(worst < 1000);  // 5 % of the buffer's time, on a host that is tens of times faster than the device
}
