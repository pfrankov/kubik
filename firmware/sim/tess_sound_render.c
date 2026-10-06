// Renders every Tess sound to a 24 kHz mono WAV, as the app would mix it (int32 accumulator clipped to s16; the
// these are raw synth levels, before Interface gain in the device mixer): event_NAME.wav and cue_NAME.wav, each
// triggered five times in a row into one file, so the variation can be heard (a sweep of the context where one
// drives the sound: the side of a tap, the level of a notch, the strength of a rub), NAME-1.wav .. NAME-3.wav (the
// first three triggers apart), all-cues.wav (the first trigger of each in turn, a listening montage), index.md (what each one measures) and rules.md (how each is composed). Prints the
// mixing cost per sample.
// Usage: tess_sound_render DIR [SEED]
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "../main/tess_sound_rules.h"
#include "tess_sound_names.h"

#define FRAME 480
#define TRIGGERS 5
#define VARIANTS 3  // the first triggers that are also written one by one
#define GAP_MS 700  // silence after each trigger in the file
enum { SWEEP_NONE, SWEEP_SIDE, SWEEP_VOLUME, SWEEP_NOTCH, SWEEP_STRENGTH, SWEEP_STRENGTH_SIDE };
typedef struct { char name[32]; bool cue; int id, sweep; } entry_t;
static entry_t s_entries[SFX_COUNT + TC_COUNT];
static int s_entry_count;

// What each sound listens to: a cue's strength and side, or an event's side, volume step or detent.
static const uint8_t k_cue_sweep[TC_COUNT] = {
    [TC_TOUCH] = SWEEP_SIDE, [TC_GLANCE] = SWEEP_SIDE, [TC_FLING] = SWEEP_STRENGTH_SIDE, [TC_CURIOUS] = SWEEP_STRENGTH_SIDE,
    [TC_PLAYFUL] = SWEEP_STRENGTH_SIDE, [TC_SCARED] = SWEEP_STRENGTH_SIDE, [TC_EXCITE] = SWEEP_STRENGTH, [TC_RUB] = SWEEP_STRENGTH,
    [TC_GRUMPY] = SWEEP_STRENGTH, [TC_SETTLE] = SWEEP_STRENGTH, [TC_IMPACT] = SWEEP_STRENGTH,
};
static int sweep_of(bool cue, int id) {
    if (cue) return k_cue_sweep[id];
    return id == SFX_TAP || id == SFX_PET || id == SFX_SURPRISE ? SWEEP_SIDE : id == SFX_VOLUME ? SWEEP_VOLUME
        : id == SFX_DETENT || id == SFX_GLINT ? SWEEP_NOTCH : SWEEP_NONE;
}
static void list_entries(void) {
    for (int i = 0; i < SFX_COUNT; i++) {
        s_entries[s_entry_count] = (entry_t){"", false, i, sweep_of(false, i)};
        snprintf(s_entries[s_entry_count++].name, sizeof s_entries->name, "event_%s", k_sfx_names[i]);
    }
    for (int i = 0; i < TC_COUNT; i++) {
        s_entries[s_entry_count] = (entry_t){"", true, i, sweep_of(true, i)};
        snprintf(s_entries[s_entry_count++].name, sizeof s_entries->name, "cue_%s", k_cue_names[i]);
    }
}
// The k-th trigger of a sweep: the context the sound listens to.
static void context(int sweep, int k, int *index, float *strength, float *position) {
    static const float k_side[TRIGGERS] = {-.9f, -.4f, 0, .4f, .9f}, k_strength[TRIGGERS] = {.2f, .4f, .6f, .8f, 1};
    static const int k_notch[TRIGGERS] = {0, 3, 5, 8, 10};
    *index = sweep == SWEEP_VOLUME ? k : sweep == SWEEP_NOTCH ? k_notch[k] : -1;
    *strength = sweep == SWEEP_STRENGTH || sweep == SWEEP_STRENGTH_SIDE ? k_strength[k] : .6f;
    *position = sweep == SWEEP_SIDE ? k_side[k] : sweep == SWEEP_STRENGTH_SIDE ? (k % 2 ? 1.f : -1.f) : 0;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 65535); put16(f, v >> 16); }
static void write_wav(const char *dir, const char *name, const int16_t *pcm, long samples) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + samples * 2); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
    put16(f, 1); put16(f, 1); put32(f, AUDIO_RATE); put32(f, AUDIO_RATE * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, samples * 2);
    fwrite(pcm, 2, (size_t)samples, f);
    fclose(f);
}

static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static double s_mix_ns, s_mix_samples;
static int s_voices;  // most voices sounding at once since the last reset of the count
// One 20 ms frame as the speaker task mixes it.
static void mix_frame(tess_sound_t *s, int16_t *out) {
    int32_t acc[FRAME] = {0};
    double t0 = now_ns();
    bool audible = tess_sound_mix(s, acc, FRAME);
    if (s->active_voices > s_voices) s_voices = s->active_voices;
    if (audible) { s_mix_ns += now_ns() - t0; s_mix_samples += FRAME; }
    for (int i = 0; i < FRAME; i++) out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}
// Plays one trigger until it has died away and the gap has passed; returns the samples in `pcm` and how long it sounded.
static long render_trigger(tess_sound_t *s, const entry_t *e, int k, int16_t *pcm, long capacity, long *sounding) {
    int index;
    float strength, position;
    context(e->sweep, k, &index, &strength, &position);
    s->gate.clock += AUDIO_RATE * 32;  // a pause long enough to clear every rate limit
    if (e->cue) tess_sound_cue(s, (tess_cue_t)e->id, strength, position);
    else tess_sound_event(s, (sfx_t)e->id, index, position, .5f);
    long n = 0;
    *sounding = 0;
    while (n + FRAME <= capacity && tess_sound_active(s)) { mix_frame(s, pcm + n); n += FRAME; }
    *sounding = n;
    if (n + FRAME > capacity) fprintf(stderr, "%s never ended\n", e->name);
    for (long quiet = 0; quiet < AUDIO_RATE * GAP_MS / 1000; quiet += FRAME, n += FRAME) mix_frame(s, pcm + n);
    return n;
}
static double db_of(double square_mean) { return 10 * log10(square_mean / (32768.0 * 32768.0) + 1e-12); }
// Loudest 100 ms in dBFS RMS, and the peak.
static void measure(const int16_t *pcm, long n, double *loudest, int *peak, int *sounding_ms) {
    const long window = AUDIO_RATE / 10;
    double sum = 0, best = 0;
    *peak = 0;
    for (long i = 0; i < n; i++) {
        sum += (double)pcm[i] * pcm[i];
        if (i >= window) sum -= (double)pcm[i - window] * pcm[i - window];
        if (sum > best) best = sum;
        if (abs(pcm[i]) > *peak) *peak = abs(pcm[i]);
    }
    *loudest = db_of(best / window);
    long first = -1, last = 0;  // the span within 35 dB of the peak
    for (long i = 0; i < n; i++) {
        if (abs(pcm[i]) <= *peak * 0.0178) continue;
        if (first < 0) first = i;
        last = i;
    }
    *sounding_ms = first < 0 ? 0 : (int)((last - first) * 1000 / AUDIO_RATE);
}

static const char *const k_note_name[5] = {"D", "E", "F#", "A", "B"};
static const char *const k_timbre_name[TIMBRE_COUNT] = {"soft", "nudge", "glow", "sweep", "pad", "cluster", "hum"};
static const char *const k_contour_name[] = {"level", "rising", "falling", "arch", "zigzag"};
static const struct { int flag; const char *text; } k_flag_text[] = {{BY_STRENGTH, "count by strength; "},
    {SCALED, "level by strength; "}, {BRIGHTEN, "deeper with strength; "}, {CAPTURE, "mic opens: dry; "},
    {HALO, "halo swell; "}};
static void write_rule(FILE *f, const char *name, const rule_t *r) {
    char notes[16] = "", flags[256] = "";
    for (int d = 0; d < 5; d++) if (r->mask >> d & 1) strcat(strcat(notes, notes[0] ? " " : ""), k_note_name[d]);
    for (unsigned i = 0; i < sizeof k_flag_text / sizeof k_flag_text[0]; i++)
        if (r->flags & k_flag_text[i].flag) strcat(flags, k_flag_text[i].text);
    if (r->pan) strcat(flags, "pitch by side; ");
    if (r->climb) strcat(flags, "pitch by level; ");
    if (r->bend) strcat(flags, r->bend > 0 ? "glides up; " : "glides down; ");
    fprintf(f, "| %s | %s | %s | %s | %d-%d | %s%d + 0-%d | %d-%d | %d | %s | %d |\n", name, k_timbre_name[r->timbre],
            k_contour_name[r->contour], notes, r->notes_min, r->notes_max, k_note_name[r->low % 5], 3 + r->low / 5, r->vary,
            r->spacing_min, r->spacing_max, r->decay_ms, flags, r->gap_ms);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s DIR [SEED]\n", argv[0]); return 1; }
    mkdir(argv[1], 0755);
    list_entries();
    static tess_sound_t s;
    static int16_t pcm[AUDIO_RATE * 90], tour[AUDIO_RATE * 300];
    long tour_n = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/index.md", argv[1]);
    FILE *index = fopen(path, "w");
    fprintf(index, "| sound | longest sounding ms (within 35 dB of peak) | loudest 100 ms per trigger (dBFS) | peak | most voices at once |\n|---|---|---|---|---|\n");
    snprintf(path, sizeof path, "%s/rules.md", argv[1]);
    FILE *rules = fopen(path, "w");
    fprintf(rules, "| sound | voice | contour | notes | count | root + random steps | ms between | note ms | flags | min gap ms |\n|---|---|---|---|---|---|---|---|---|---|\n");
    tess_sound_seed(&s, argc > 2 ? (unsigned)atoi(argv[2]) : 20260930);
    tess_sound_reset(&s);
    for (int i = 0; i < s_entry_count; i++) {
        const entry_t *e = &s_entries[i];
        long n = 0;
        char levels[128] = "";
        int worst_peak = 0, longest_ms = 0;
        s_voices = 0;
        for (int k = 0; k < TRIGGERS; k++) {
            long sounding, from = n;
            n += render_trigger(&s, e, k, pcm + n, (long)(sizeof pcm / 2) - n, &sounding);
            double db;
            int peak, ms;
            measure(pcm + from, n - from, &db, &peak, &ms);
            longest_ms = ms > longest_ms ? ms : longest_ms;
            worst_peak = peak > worst_peak ? peak : worst_peak;
            snprintf(levels + strlen(levels), sizeof levels - strlen(levels), "%s%.1f", k ? ", " : "", db);
            if (k < VARIANTS) {
                char variant[64];
                snprintf(variant, sizeof variant, "%s-%d", e->name, k + 1);
                write_wav(argv[1], variant, pcm + from, n - from);
            }
            if (k == 0) { memcpy(tour + tour_n, pcm + from, (size_t)(n - from) * 2); tour_n += n - from; }
        }
        write_wav(argv[1], e->name, pcm, n);
        fprintf(index, "| %s | %d | %s | %d | %d |\n", e->name, longest_ms, levels, worst_peak, s_voices);
        write_rule(rules, e->name, e->cue ? &k_cue_rules[e->id] : &k_event_rules[e->id]);
    }
    write_wav(argv[1], "all-cues", tour, tour_n);
    fclose(index);
    fclose(rules);
    printf("%d sounds x %d triggers, tour %.1f s, mixing %.0f ns per sample while sounding\n", s_entry_count, TRIGGERS,
           (double)tour_n / AUDIO_RATE, s_mix_ns / s_mix_samples);
    return 0;
}
