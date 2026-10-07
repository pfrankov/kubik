// The host-side synth cost probe, kept separate so a slow buffer can be replayed diagnostically.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "tess_sound_probe.h"

enum { COST_REPLAYS = 5 };
enum { ORIGINAL_WORST_PLAY = 11, ORIGINAL_WORST_FRAME = 118 };

typedef struct {
    double leading_us, mix_wall_us, trailing_us, total_wall_us, cpu_us;
} cost_timing_t;

typedef struct {
    double us;
    int play, frame;
} clock_peak_t;

typedef struct {
    int play, frame, seed, voices, pending;
    const char *sound;
    unsigned reverb;
    uint64_t signature;
    cost_timing_t timing;
} cost_record_t;

typedef struct {
    double worst, total;
    long buffers, slow_buffers;
    cost_record_t worst_record, first_slow, max_slow, original_worst;
    bool found_first_slow, found_original_worst;
    clock_peak_t max_leading, max_trailing, max_clock_brackets;
} cost_stats_t;

static double elapsed_us(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1e6 + (end.tv_nsec - start.tv_nsec) / 1e3;
}

static cost_timing_t timed_mix(tess_sound_t *sound, int32_t *frame) {
    struct timespec wall0, cpu0, wall1, wall2, cpu1, wall3;
    assert(clock_gettime(CLOCK_MONOTONIC, &wall0) == 0);
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &wall1) == 0);
    tess_sound_mix(sound, frame, PROBE_FRAME);
    assert(clock_gettime(CLOCK_MONOTONIC, &wall2) == 0);
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &wall3) == 0);
    return (cost_timing_t){
        .leading_us = elapsed_us(wall0, wall1),
        .mix_wall_us = elapsed_us(wall1, wall2),
        .trailing_us = elapsed_us(wall2, wall3),
        .total_wall_us = elapsed_us(wall0, wall3),
        .cpu_us = elapsed_us(cpu0, cpu1),
    };
}

static void remember_clock_peak(clock_peak_t *peak, double us, int play, int frame) {
    if (us > peak->us) *peak = (clock_peak_t){.us = us, .play = play, .frame = frame};
}

static cost_record_t cost_record(const tess_sound_t *sound, int play, int frame, const char *name,
                                 cost_timing_t timing, uint64_t signature) {
    return (cost_record_t){
        .play = play, .frame = frame, .seed = 77 + play,
        .voices = sound->active_voices,
        .pending = sound->pending_count - sound->pending_head,
        .sound = name, .reverb = sound->reverb_samples_left,
        .signature = signature, .timing = timing,
    };
}

static void print_cost_record(const char *label, const cost_record_t *record) {
    const cost_timing_t *timing = &record->timing;
    fprintf(stderr,
            "synth: %s play %d seed %d %s frame %d: CPU %.1f us, wall1-wall0 %.1f us, "
            "wall2-wall1 mix-only %.1f us, wall3-wall2 %.1f us, wall3-wall0 total %.1f us; "
            "after mix: %d voices, %d pending, %u reverb samples, PCM %016" PRIx64 "\n",
            label, record->play, record->seed, record->sound, record->frame, timing->cpu_us,
            timing->leading_us, timing->mix_wall_us, timing->trailing_us, timing->total_wall_us,
            record->voices, record->pending, record->reverb, record->signature);
}

static void print_clock_peak(const char *label, const clock_peak_t *peak) {
    fprintf(stderr, "synth: max %s CPU-clock bracket wall interval %.1f us at play %d frame %d\n",
            label, peak->us, peak->play, peak->frame);
}

static uint64_t frame_signature(const int32_t *frame);

static void record_cost_sample(cost_stats_t *stats, const tess_sound_t *sound, int play, int frame,
                               const char *name, cost_timing_t timing, const int32_t *pcm) {
    bool original = play == ORIGINAL_WORST_PLAY && frame == ORIGINAL_WORST_FRAME;
    if (timing.cpu_us > stats->worst || timing.cpu_us >= 1000 || original) {
        cost_record_t record = cost_record(sound, play, frame, name, timing, frame_signature(pcm));
        if (timing.cpu_us > stats->worst) {
            stats->worst = timing.cpu_us;
            stats->worst_record = record;
        }
        if (timing.cpu_us >= 1000) {
            if (!stats->found_first_slow) { stats->first_slow = record; stats->found_first_slow = true; }
            if (timing.cpu_us > stats->max_slow.timing.cpu_us) stats->max_slow = record;
            stats->slow_buffers++;
        }
        if (original) {
            stats->original_worst = record;
            stats->found_original_worst = true;
        }
    }
    remember_clock_peak(&stats->max_leading, timing.leading_us, play, frame);
    remember_clock_peak(&stats->max_trailing, timing.trailing_us, play, frame);
    remember_clock_peak(&stats->max_clock_brackets, timing.leading_us + timing.trailing_us, play, frame);
    stats->total += timing.cpu_us;
    stats->buffers++;
}

static void print_cost_stats(const cost_stats_t *stats) {
    fprintf(stderr, "synth: %.1f us CPU per 20 ms buffer on average, %.1f us at worst (host thread)\n",
            stats->total / stats->buffers, stats->worst);
    print_cost_record("worst input", &stats->worst_record);
    fprintf(stderr, "synth: buffers at or above 1000 us CPU: %ld\n", stats->slow_buffers);
    if (stats->found_first_slow) print_cost_record("first over-limit buffer", &stats->first_slow);
    if (stats->slow_buffers) print_cost_record("maximum over-limit buffer", &stats->max_slow);
    if (stats->found_original_worst) print_cost_record("previously reported worst frame", &stats->original_worst);
    else fprintf(stderr, "synth: previously reported worst frame play %d frame %d not observed\n",
                 ORIGINAL_WORST_PLAY, ORIGINAL_WORST_FRAME);
    print_clock_peak("leading", &stats->max_leading);
    print_clock_peak("trailing", &stats->max_trailing);
    print_clock_peak("combined", &stats->max_clock_brackets);
    fputs("synth: bracket intervals include adjacent MONOTONIC reads; added timer calls can perturb the CPU measurement\n", stderr);
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
        cost_timing_t timing = timed_mix(sound, frame);
        uint64_t signature = frame_signature(frame);
        fprintf(stderr,
                "synth: replay %d/%d CPU %.1f us, wall1-wall0 %.1f us, wall2-wall1 mix-only %.1f us, "
                "wall3-wall2 %.1f us, wall3-wall0 total %.1f us, PCM %s\n",
                replay, COST_REPLAYS, timing.cpu_us, timing.leading_us, timing.mix_wall_us,
                timing.trailing_us, timing.total_wall_us, signature == expected ? "match" : "MISMATCH");
    }
}

void check_cost(tess_sound_t *sound) {
    // Keep one pristine state for failure-only reconstruction; bracket timing is intentional in this diagnostic.
    tess_sound_t initial = *sound;
    int32_t frame[PROBE_FRAME];
    tess_sound_reset(sound);
    probe_rest(sound);
    cost_stats_t stats = {0};
    for (int play = 0; play < 40; play++) {
        start_cost_play(sound, play);
        const char *sound_name = play % 2 ? "TC_LOVE" : "SFX_SETUP_OK";
        int frame_no = 0;
        while (tess_sound_active(sound)) {
            memset(frame, 0, sizeof frame);
            cost_timing_t timing = timed_mix(sound, frame);
            record_cost_sample(&stats, sound, play, frame_no, sound_name, timing, frame);
            frame_no++;
        }
    }
    print_cost_stats(&stats);
    if (stats.worst >= 1000) {
        tess_sound_t target_state;
        bool matches = reconstruct_cost_frame(sound, &initial, stats.worst_record.play, stats.worst_record.frame,
                                              stats.worst_record.signature, &target_state);
        fprintf(stderr, "synth: deterministic worst-frame reconstruction PCM %s\n", matches ? "match" : "MISMATCH");
        if (matches) replay_cost_frame(sound, &target_state, stats.worst_record.signature);
    }
    assert(stats.worst < 1000);  // 5 % of the buffer's time, on a host that is tens of times faster than the device
}
