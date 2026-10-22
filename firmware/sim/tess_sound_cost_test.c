// The host-side synth cost probe; a buffer over budget is re-measured in place before the probe fails.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#include "tess_sound_probe.h"

// Budget of one 20 ms buffer, replays of each over-budget buffer, over-budget buffers re-measured.
enum { COST_BUDGET_US = 1000, COST_REPLAYS = 5, COST_SLOW_KEPT = 4 };

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
    tess_tone_t ring = {.kind = TT_RING, .hz = 260.f + play * 3, .gain = .7f, .shade = .6f};
    assert(tess_sound_tone(sound, &ring)); // finite body note plus the foreground word, same production mixer
}

// Re-runs the same input from the pristine state with the same per-buffer measurement as the
// first pass, so the slow buffer is timed in place after the same input history.
static mix_cost_t remeasure_cost_frame(tess_sound_t *sound, const tess_sound_t *initial, int target_play,
                                       int target_frame, uint64_t expected) {
    int32_t frame[PROBE_FRAME];
    *sound = *initial;
    tess_sound_reset(sound);
    probe_rest(sound);
    for (int play = 0; play <= target_play; play++) {
        start_cost_play(sound, play);
        for (int frame_no = 0; tess_sound_active(sound); frame_no++) {
            memset(frame, 0, sizeof frame);
            mix_cost_t cost = measure_mix(sound, frame);
            if (play != target_play || frame_no != target_frame) continue;
            bool matches = frame_signature(frame) == expected;
            fprintf(stderr, "synth: replay CPU %.1f us, monotonic %.1f us, PCM %s\n",
                    cost.cpu_us, cost.wall_us, matches ? "match" : "MISMATCH");
            report_phases("replay", cost);
            assert(matches);
            return cost;
        }
    }
    fprintf(stderr, "synth: replay did not reach play %d frame %d\n", target_play, target_frame);
    abort();
}

// Median CPU of the replays; an odd count keeps it a measured value. One timing spike on a
// shared CI runner cannot fail the probe, a buffer that is slow in most replays does.
static double replay_median_us(tess_sound_t *sound, const tess_sound_t *initial, int target_play,
                               int target_frame, uint64_t expected) {
    double cpu[COST_REPLAYS];
    for (int replay = 0; replay < COST_REPLAYS; replay++) {
        double cpu_us = remeasure_cost_frame(sound, initial, target_play, target_frame, expected).cpu_us;
        int slot = replay;  // insertion keeps the replays sorted
        for (; slot > 0 && cpu[slot - 1] > cpu_us; slot--) cpu[slot] = cpu[slot - 1];
        cpu[slot] = cpu_us;
    }
    return cpu[COST_REPLAYS / 2];
}

typedef struct {
    int play, frame;
    double cpu_us;
    uint64_t signature;
} slow_frame_t;

typedef struct {
    slow_frame_t frames[COST_SLOW_KEPT];
    int kept, total;
} slow_frames_t;

// Keeps the slowest over-budget buffers; `total` still counts every one of them.
static void note_slow_frame(slow_frames_t *slow, slow_frame_t frame) {
    slow->total++;
    int slot = slow->kept;
    if (slot == COST_SLOW_KEPT) {
        if (slow->frames[slot - 1].cpu_us >= frame.cpu_us) return;
        slot--;  // replace the fastest kept buffer
    } else {
        slow->kept++;
    }
    for (; slot > 0 && slow->frames[slot - 1].cpu_us < frame.cpu_us; slot--) slow->frames[slot] = slow->frames[slot - 1];
    slow->frames[slot] = frame;
}

// Returns how many over-budget buffers stay over budget by the median of their replays.
static int recheck_slow_frames(tess_sound_t *sound, const tess_sound_t *initial, const slow_frames_t *slow) {
    int confirmed = 0;
    for (int i = 0; i < slow->kept; i++) {
        const slow_frame_t *slow_frame = &slow->frames[i];
        fprintf(stderr, "synth: over budget play %d frame %d: first CPU %.1f us; replaying %d times\n",
                slow_frame->play, slow_frame->frame, slow_frame->cpu_us, COST_REPLAYS);
        double median = replay_median_us(sound, initial, slow_frame->play, slow_frame->frame, slow_frame->signature);
        fprintf(stderr, "synth: play %d frame %d: first CPU %.1f us, replay median %.1f us (budget %d us)\n",
                slow_frame->play, slow_frame->frame, slow_frame->cpu_us, median, COST_BUDGET_US);
        if (median >= COST_BUDGET_US) confirmed++;
    }
    return confirmed;
}

void check_cost(tess_sound_t *sound, bool enforce_budget) {
    // Keep one pristine sound state for reconstruction of over-budget buffers.
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
    slow_frames_t slow = {0};
    for (int play = 0; play < 40; play++) {
        start_cost_play(sound, play);
        const char *sound_name = play % 2 ? "TC_LOVE" : "SFX_SETUP_OK";
        int frame_no = 0;
        while (tess_sound_active(sound)) {
            memset(frame, 0, sizeof frame);
            mix_cost_t cost = measure_mix(sound, frame);
            double cpu_us = cost.cpu_us;
            if (cpu_us >= COST_BUDGET_US)
                note_slow_frame(&slow, (slow_frame_t){play, frame_no, cpu_us, frame_signature(frame)});
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
    if (slow.total == 0) return;
    fprintf(stderr, "synth: %d of %ld buffers over %d us on first measurement; retained the slowest %d\n",
            slow.total, buffers, COST_BUDGET_US, slow.kept);
    if (enforce_budget) assert(slow.total <= COST_SLOW_KEPT);  // Widespread overruns fail before replays.
    int confirmed = recheck_slow_frames(sound, &initial, &slow);
    fprintf(stderr, "synth: %d re-measured buffers stay over budget\n", confirmed);
    if (!enforce_budget) return;  // Sanitizer build: PCM determinism only, its timing is not the synth's.
    assert(confirmed == 0);  // Strict per-buffer budget, measured without instrumentation.
}
