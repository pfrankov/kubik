// Host helpers of the Tess sound test: render one sound to PCM and measure it. Header only; test code.
#pragma once
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../main/tess_sound.h"

#define PROBE_PI 3.14159265358979323846
#define PROBE_SQRT2 1.41421356237309504880
#define PROBE_FRAME 480                    // 20 ms, as the speaker task mixes
#define PROBE_MAX_SAMPLES (AUDIO_RATE * 7)  // no sound may sound (with its tail) longer than this
typedef struct {
    int32_t *pcm;
    long length;
    int peak_voices;  // most voices sounding at once
} probe_t;

// One 20 ms frame of `s` added into pcm; keeps count of the voices.
static inline void probe_mix(tess_sound_t *s, int32_t *pcm, int *peak_voices) {
    tess_sound_mix(s, pcm, PROBE_FRAME);
    if (peak_voices && s->active_voices > *peak_voices) *peak_voices = s->active_voices;
}
// Mixes until the sound has died away (asserts it does within PROBE_MAX_SAMPLES).
static inline void probe_finish(tess_sound_t *s, probe_t *p) {
    while (tess_sound_active(s)) {
        assert(p->length + PROBE_FRAME <= PROBE_MAX_SAMPLES);
        probe_mix(s, p->pcm + p->length, &p->peak_voices);
        p->length += PROBE_FRAME;
    }
}
static inline probe_t probe_event(tess_sound_t *s, int32_t *storage, sfx_t event, int index, float position) {
    probe_t p = {storage, 0, 0};
    memset(storage, 0, sizeof(int32_t) * PROBE_MAX_SAMPLES);
    tess_sound_event(s, event, index, position, .5f);
    probe_finish(s, &p);
    return p;
}
static inline probe_t probe_cue(tess_sound_t *s, int32_t *storage, tess_cue_t cue, float strength, float position) {
    probe_t p = {storage, 0, 0};
    memset(storage, 0, sizeof(int32_t) * PROBE_MAX_SAMPLES);
    tess_sound_cue(s, cue, strength, position);
    probe_finish(s, &p);
    return p;
}
// Silence of `frames` 20 ms buffers.
static inline void probe_silence(tess_sound_t *s, int frames) {
    int32_t scratch[PROBE_FRAME];
    for (int i = 0; i < frames; i++) { memset(scratch, 0, sizeof scratch); tess_sound_mix(s, scratch, PROBE_FRAME); }
}
// Silence long enough to clear every rate limit of the events (10 s).
static inline void probe_rest(tess_sound_t *s) { probe_silence(s, 500); }
// ... and of the cues (32 s clears their rate limits).
static inline void probe_rest_long(tess_sound_t *s) { probe_silence(s, 1600); }

static inline double probe_db(double rms) { return 20 * log10(rms / 32768 + 1e-9); }
static inline int probe_peak(const probe_t *p) {
    int peak = 0;
    for (long i = 0; i < p->length; i++) if (abs(p->pcm[i]) > peak) peak = abs(p->pcm[i]);
    return peak;
}
// The loudest 100 ms, in dBFS RMS (a shorter sound counts as padded with silence): how the loudness classes are defined.
static inline double probe_loudest_100ms(const probe_t *p) {
    const long window = AUDIO_RATE / 10;
    double best = 0, sum = 0;
    for (long i = 0; i < p->length; i++) {
        sum += (double)p->pcm[i] * p->pcm[i];
        if (i >= window) sum -= (double)p->pcm[i - window] * p->pcm[i - window];
        if (sum > best) best = sum;
    }
    return probe_db(sqrt(best / window));
}
// A-weighted loudest 100 ms, for short tonal UI sounds. Host diagnostics only.
// Bilinear transform of the A-weighting poles at 24 kHz, normalized at 1 kHz.
static inline double probe_weighted_100ms(const probe_t *p, bool speaker) {
    static const double filter[4][5] = {
        {0.4256263892891057, 0.8512527785782114, 0.4256263892891057, 0.4592980869736559, 0.052738683174415},
        {1, -2, 1, -1.796050696686541, 0.8009464255474772},
        {1, -2, 1, -1.989243394529075, 0.9892723206693893},
        {0.8391686399605895, -1.678337279921179, 0.8391686399605895, -1.664245930525056, 0.6924286293173021},
    };
    double state[4][2] = {{0}}, ring[AUDIO_RATE / 10] = {0};
    double sum = 0, best = 0;
    const int window = AUDIO_RATE / 10;
    for (long i = 0; i < p->length; i++) {
        double x = p->pcm[i];
        for (int f = 0; f < (speaker ? 4 : 3); f++) {
            const double *c = filter[f];
            double y = c[0] * x + state[f][0];
            state[f][0] = c[1] * x - c[3] * y + state[f][1];
            state[f][1] = c[2] * x - c[4] * y;
            x = y;
        }
        int slot = i % window;
        sum += x * x - ring[slot]; ring[slot] = x * x;
        if (sum > best) best = sum;
    }
    return probe_db(sqrt(best / window));
}
// Approximate small-speaker projection: two 700 Hz high-pass poles. The physical
// low/high slider recording showed ~12 dB extra loss at the low end.
static inline double probe_perceived_100ms(const probe_t *p) {
    return probe_weighted_100ms(p, true);
}
// The worst click: the sharpest bend (second difference) against the bends of the 48 samples before it plus 3 % of the
// peak. A smooth sound, however bright, bends steadily; a cut or a jump is a spike out of nowhere.
static inline double probe_worst_click(const probe_t *p) {
    double worst = 0, floor = probe_peak(p) * .03, energy = 0;
    enum { LOOK = 48 };
    for (long i = 2; i < p->length; i++) {
        double bend = fabs((double)p->pcm[i] - 2. * p->pcm[i - 1] + p->pcm[i - 2]);
        double typical = sqrt(energy / LOOK);
        if (i > LOOK + 2 && bend / (typical + floor) > worst) worst = bend / (typical + floor);
        energy += bend * bend;
        if (i >= LOOK + 2) {
            double old = (double)p->pcm[i - LOOK] - 2. * p->pcm[i - LOOK - 1] + p->pcm[i - LOOK - 2];
            energy -= old * old;
        }
    }
    return worst;
}
// Samples until the sound is first as loud as `fraction` of its peak.
static inline long probe_rise(const probe_t *p, double fraction) {
    int peak = probe_peak(p);
    long i = 0;
    while (i < p->length && abs(p->pcm[i]) < peak * fraction) i++;
    return i;
}
// Milliseconds until the wave first reaches half of the loudest it gets in the first 150 ms: how soon it is there (a
// strike is under a few ms, a pad's slow swell is a hundred).
static inline int probe_onset_ms(const probe_t *p) {
    long window = AUDIO_RATE * 150 / 1000 < p->length ? AUDIO_RATE * 150 / 1000 : p->length;
    int peak = 0;
    for (long i = 0; i < window; i++) if (abs(p->pcm[i]) > peak) peak = abs(p->pcm[i]);
    long i = 0;
    while (i < window && abs(p->pcm[i]) < peak / 2) i++;
    return (int)(i * 1000 / AUDIO_RATE);
}
// Milliseconds from the first to the last sample within 35 dB of the peak.
static inline int probe_sounding_ms(const probe_t *p) {
    int limit = (int)(probe_peak(p) * 0.0178), first = -1;
    long last = 0;
    for (long i = 0; i < p->length; i++) {
        if (abs(p->pcm[i]) <= limit) continue;
        if (first < 0) first = (int)i;
        last = i;
    }
    return first < 0 ? 0 : (int)((last - first) * 1000 / AUDIO_RATE);
}
static inline uint64_t probe_signature(const probe_t *p) {
    uint64_t hash = 1;
    for (long i = 0; i < p->length; i++) hash = hash * 33 + (uint32_t)p->pcm[i];
    return hash;
}
// Power near `hz` (Goertzel over one Hann-windowed frame).
static inline double probe_bin(const int32_t *x, int n, double hz) {
    double coeff = 2 * cos(2 * PROBE_PI * hz / AUDIO_RATE), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double w = .5 - .5 * cos(2 * PROBE_PI * i / (n - 1)), s0 = x[i] * w + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}
static inline double probe_centroid(const probe_t *p) {
    double weighted = 0, total = 0;
    for (int midi = 40; midi <= 108; midi++) {
        double hz = 440 * pow(2, (midi - 69) / 12.), power = 0;
        for (long start = 0; start + 4096 <= p->length + 2048; start += 2048) {
            int32_t frame[4096] = {0};
            for (int i = 0; i < 4096 && start + i < p->length; i++) frame[i] = p->pcm[start + i];
            power += probe_bin(frame, 4096, hz);
        }
        weighted += hz * power;
        total += power;
    }
    return total > 0 ? weighted / total : 0;
}

// The share of the sound's energy above `hz`: two cascaded 2nd-order high-passes (Butterworth, 24 dB per octave) over the
// whole render. The "no high ringing" measure.
static inline double probe_high_share(const probe_t *p, double hz) {
    double k = tan(PROBE_PI * hz / AUDIO_RATE), norm = 1 / (1 + PROBE_SQRT2 * k + k * k);
    double b0 = norm, b1 = -2 * norm, a1 = 2 * (k * k - 1) * norm, a2 = (1 - PROBE_SQRT2 * k + k * k) * norm;
    double z[2][2] = {{0, 0}, {0, 0}}, high = 0, total = 0;
    for (long i = 0; i < p->length; i++) {
        double x = p->pcm[i], y = x;
        total += x * x;
        for (int stage = 0; stage < 2; stage++) {  // (direct form II transposed)
            double out = b0 * y + z[stage][0];
            z[stage][0] = b1 * y - a1 * out + z[stage][1];
            z[stage][1] = b0 * y - a2 * out;
            y = out;
        }
        high += y * y;
    }
    return total > 0 ? high / total : 0;
}

// A radix-2 FFT of 4096 points (in place), for counting the partials a sound has.
enum { PROBE_FFT = 4096 };
static inline void probe_fft(double *re, double *im) {
    for (int i = 1, j = 0; i < PROBE_FFT; i++) {
        int bit = PROBE_FFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= PROBE_FFT; len <<= 1) {
        double angle = -2 * PROBE_PI / len;
        for (int i = 0; i < PROBE_FFT; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(angle * k), wi = sin(angle * k);
                int a = i + k, b = i + k + len / 2;
                double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
    }
}
// The magnitude spectrum (bins 0..2047) of the 4096-sample Hann-windowed frame from `start`.
static inline void probe_spectrum(const probe_t *p, long start, double *mag) {
    static double re[PROBE_FFT], im[PROBE_FFT];
    for (int i = 0; i < PROBE_FFT; i++) {
        re[i] = start + i < p->length ? p->pcm[start + i] * (.5 - .5 * cos(2 * PROBE_PI * i / (PROBE_FFT - 1))) : 0;
        im[i] = 0;
    }
    probe_fft(re, im);
    for (int i = 0; i < PROBE_FFT / 2; i++) mag[i] = hypot(re[i], im[i]);
}
// The partials standing in one frame: spectral peaks between 80 Hz and 3 kHz within 20 dB of the frame's strongest, at
// least 20 Hz apart (a detuned copy a few hertz away is one peak: the layers are).
static inline int probe_frame_partials(const probe_t *p, long start) {
    static double mag[PROBE_FFT / 2];
    probe_spectrum(p, start, mag);
    double strongest = 0;
    for (int i = 0; i < PROBE_FFT / 2; i++) {
        double hz = (double)i * AUDIO_RATE / PROBE_FFT;
        if (hz >= 80 && hz <= 3000 && mag[i] > strongest) strongest = mag[i];
    }
    int count = 0, last = -100;
    for (int i = 2; i < PROBE_FFT / 2 - 2; i++) {
        double hz = (double)i * AUDIO_RATE / PROBE_FFT;
        double around = fmax(fmax(mag[i - 1], mag[i + 1]), fmax(mag[i - 2], mag[i + 2]));
        bool standing = hz >= 80 && hz <= 3000 && mag[i] >= strongest * .1 && mag[i] >= around;
        if (!standing || (i - last) * AUDIO_RATE / PROBE_FFT < 20) continue;
        count++;
        last = i;
    }
    return count;
}
// Most partials standing at once in any frame of the render (hop 1024).
static inline int probe_partials(const probe_t *p) {
    int most = 0;
    for (long start = 0; start < p->length; start += 1024) {
        int count = probe_frame_partials(p, start);
        if (count > most) most = count;
    }
    return most;
}

// The spectral flatness (geometric over arithmetic mean of the power between 80 Hz and 3 kHz) of the noisiest frame that
// is within 20 dB of the loudest: about .5 for noise, a few thousandths for a few sines. The "no noise" measure.
static inline double probe_frame_energy(const probe_t *p, long start) {
    double energy = 0;
    for (long i = start; i < start + PROBE_FFT && i < p->length; i++) energy += (double)p->pcm[i] * p->pcm[i];
    return energy;
}
static inline double probe_flatness(const probe_t *p) {
    static double mag[PROBE_FFT / 2];
    double loudest = 0, worst = 0;
    for (long start = 0; start < p->length; start += 1024) loudest = fmax(loudest, probe_frame_energy(p, start));
    for (long start = 0; start < p->length; start += 1024) {
        if (probe_frame_energy(p, start) < loudest * .01) continue;
        probe_spectrum(p, start, mag);
        double log_sum = 0, sum = 0;
        int bins = 0;
        for (int i = 0; i < PROBE_FFT / 2; i++) {
            double hz = (double)i * AUDIO_RATE / PROBE_FFT;
            if (hz < 80 || hz > 3000) continue;
            log_sum += log(mag[i] * mag[i] + 1e-9);
            sum += mag[i] * mag[i];
            bins++;
        }
        double flatness = exp(log_sum / bins) / (sum / bins + 1e-9);
        if (flatness > worst) worst = flatness;
    }
    return worst;
}

static inline uint32_t probe_voice_end(const tess_voice_t *v) {
    double release_steps = log((1 << 22) / (double)v->sustain) / log(v->release / (double)(1 << 24));
    return v->start + 2 * v->hold_left + 2 * (uint32_t)(((1LL << 30) / v->attack_step) + ceil(release_steps) + 1);
}
