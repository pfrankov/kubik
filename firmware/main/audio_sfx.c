#include "audio.h"
#include "audio_sfx.h"
#include "audio_levels.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "tess_sound.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "audio";
// ---------------------------------------------------------------- SFX
// Plush's sounds are PCM in the memory-mapped assets partition (4-byte aligned); Tess's are synthesized (tess_sound.h).
// There are no spoken prompts: Kubik has no fixed language, so status and
// errors are shown on screen (face.h bubbles) with a sound.
static const char *const k_sfx_names[SFX_COUNT] = {
    "boot",  "listen_start", "latch_start", "listen_stop", "think", "notify",   "error",  "not_heard", "connect",
    "disconnect", "wake", "tap", "giggle", "pet", "dizzy", "surprise", "volume",
    "hello", "screen_on", "screen_off", "setup", "setup_phone", "setup_wait", "setup_ok", "setup_fail",
    "menu_open", "menu_close", "detent", "glint", "arm", "disarm", "deny",
    "page", "dismiss", "power_off"};

#define MAX_VARIANTS 11  // the slider notches: 0..100 % in 10 % steps
typedef struct {
    const int16_t *pcm[MAX_VARIANTS];
    uint32_t len[MAX_VARIANTS];
    uint8_t n, last, last_key;
} sfx_group_t;

// A sound never plays exactly the same twice: each play also takes one of a few keys
// (never the previous one). The whole figure moves, so its intervals, rhythm and
// envelope stay its own; the range is narrow enough to remain one bear. Q16 rates.
static const uint32_t k_sfx_keys[] = {
    55109,  // -3 semitones
    61858,  // -1
    65536,  //  0
    73562,  // +2
};
#define SFX_KEYS (sizeof(k_sfx_keys) / sizeof(k_sfx_keys[0]))
static sfx_group_t s_sfx[SFX_COUNT];
static SemaphoreHandle_t s_sfx_mtx;
static tess_sound_t *s_tess;
static float s_tess_position, s_tess_energy;

#define MAX_VOICES 3
typedef struct {
    const int16_t *p;
    uint32_t left;      // output samples still to play
    uint32_t pos, step;  // Q16 read position and rate in p
} voice_t;
static voice_t s_voices[MAX_VOICES];

static bool lock_audible(void) {
    if (!s_sfx_mtx) return false;
    xSemaphoreTake(s_sfx_mtx, portMAX_DELAY);
    if (audio_levels_interface_enabled()) return true;
    xSemaphoreGive(s_sfx_mtx);
    return false;
}

static void load_kit(void) {
    char name[32];
    for (int g = 0; g < SFX_COUNT; g++) {
        sfx_group_t *k = &s_sfx[g];
        snprintf(name, sizeof(name), "sfx/%s", k_sfx_names[g]);
        const int16_t *p = assets_pcm(name, &k->len[0]);
        if (p) {
            k->pcm[0] = p;
            k->n = 1;
        }
        for (int v = 1; v <= MAX_VARIANTS && k->n < MAX_VARIANTS; v++) {
            snprintf(name, sizeof(name), "sfx/%s_%d", k_sfx_names[g], v);
            p = assets_pcm(name, &k->len[k->n]);
            if (!p) break;
            k->pcm[k->n++] = p;
        }
        if (!k->n) ESP_LOGW(TAG, "sound kit: no %s", k_sfx_names[g]);
    }
}

void audio_sfx_quiet(void) {
    if (!s_sfx_mtx) return;
    xSemaphoreTake(s_sfx_mtx, portMAX_DELAY);
    if (KUBIK_CHARACTER == CHARACTER_TESS && s_tess) tess_sound_reset(s_tess);
    memset(s_voices, 0, sizeof s_voices);
    xSemaphoreGive(s_sfx_mtx);
}
void audio_tess_gesture(float x, float y) {
    if (KUBIK_CHARACTER != CHARACTER_TESS) return;
    s_tess_position = (x - 240) / 240;
    s_tess_energy = 1 - y / 480;
}
static void tess_event(sfx_t id, int index) {
    if (tess_sound_event_is_utterance(id) && (audio_mic_is_open() || audio_stream_playing())) return;
    if (!lock_audible()) return;
    if (KUBIK_CHARACTER == CHARACTER_TESS && s_tess) {
        tess_sound_event(s_tess, id, index, s_tess_position, s_tess_energy);
    }
    xSemaphoreGive(s_sfx_mtx);
    audio_kick();
}
void audio_tess_cue(tess_cue_t cue, float strength, float position) {
    if (KUBIK_CHARACTER != CHARACTER_TESS || !s_sfx_mtx) return;
    // The words of a mood (TC_CURIOUS on) are never said over the microphone or a spoken reply.
    if ((cue >= TC_CURIOUS || tess_sound_cue_is_utterance(cue)) && (audio_mic_is_open() || audio_stream_playing())) return;
    if (!lock_audible()) return;
    bool played = false;
    if (s_tess) {
        played = tess_sound_cue(s_tess, cue, strength, position);
    }
    xSemaphoreGive(s_sfx_mtx);
    if (played) audio_kick();
}

static void play_pcm(const int16_t *p, uint32_t len, uint32_t step) {
    if (!p || !len || !lock_audible()) return;
    int slot = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (s_voices[i].left == 0) {
            slot = i;
            break;
        }
        if (s_voices[i].left < s_voices[slot].left) slot = i;  // all busy: replace the one closest to its end
    }
    s_voices[slot].p = p;
    s_voices[slot].pos = 0;
    s_voices[slot].step = step;
    // Linear interpolation reads sample i and i + 1: stop before the last one.
    s_voices[slot].left = len > 1 ? (uint32_t)(((uint64_t)(len - 1) << 16) / step) : 0;
    xSemaphoreGive(s_sfx_mtx);
    audio_kick();
}

void audio_sfx(sfx_t id) {
    if ((unsigned)id >= SFX_COUNT) return;
    if (KUBIK_CHARACTER == CHARACTER_TESS) { tess_event(id, -1); return; }
    sfx_group_t *k = &s_sfx[id];
    if (!k->n) return;
    int v = 0;
    if (k->n > 1) {
        v = (int)(esp_random() % (k->n - 1));
        if (v >= k->last) v++;  // never the same variant twice in a row
    }
    k->last = (uint8_t)v;
    int key = (int)(esp_random() % (SFX_KEYS - 1));
    if (key >= k->last_key) key++;
    k->last_key = (uint8_t)key;
    play_pcm(k->pcm[v], k->len[v], k_sfx_keys[key]);
}

void audio_sfx_level(sfx_t id, int index) {
    if ((unsigned)id >= SFX_COUNT) return;
    if (KUBIK_CHARACTER == CHARACTER_TESS) { tess_event(id, index); return; }
    sfx_group_t *k = &s_sfx[id];
    if (!k->n) return;
    int v = index < 0 ? 0 : index >= k->n ? k->n - 1 : index;
    play_pcm(k->pcm[v], k->len[v], 65536);  // the pitch is the level here: never transposed
}

void audio_sfx_volume(int level) { audio_sfx_level(SFX_VOLUME, level - 1); }

bool audio_sfx_playing(void) {
    if (!lock_audible()) return false;
    bool active = KUBIK_CHARACTER == CHARACTER_TESS && s_tess && tess_sound_active(s_tess);
    if (KUBIK_CHARACTER == CHARACTER_PLUSH)
        for (int i = 0; i < MAX_VOICES; i++) if (s_voices[i].left) active = true;
    xSemaphoreGive(s_sfx_mtx);
    return active;
}

static volatile int64_t s_self_until;  // us: own sounds audible until then
bool audio_self_audible(void) { return esp_timer_get_time() < s_self_until; }

// Mixes the SFX voices into acc.
void audio_sfx_mix(int32_t *acc, int n) {
    if (!lock_audible()) return;
    if (KUBIK_CHARACTER == CHARACTER_TESS) tess_sound_mix(s_tess, acc, n);
    else for (int k = 0; k < MAX_VOICES; k++) {
        voice_t *v = &s_voices[k];
        if (!v->left) continue;
        int m = v->left < (uint32_t)n ? (int)v->left : n;
        uint32_t pos = v->pos, step = v->step;
        for (int i = 0; i < m; i++, pos += step) {
            const int16_t *q = v->p + (pos >> 16);
            int32_t fr = (int32_t)(pos & 0xFFFF) >> 1;  // Q15: the product stays within int32
            acc[i] += q[0] + (((q[1] - q[0]) * fr) >> 15);
        }
        v->p += pos >> 16;  // keep the Q16 position small
        v->pos = pos & 0xFFFF;
        v->left -= m;
    }
    xSemaphoreGive(s_sfx_mtx);

}

void audio_sfx_note_output(bool audible) {
    // Include queued DMA frames and room decay; muted frames never extend microphone suppression.
    if (audible) s_self_until = esp_timer_get_time() + 20000 + 60000 + 80000;
}


void audio_sfx_init(void) {
    s_sfx_mtx = xSemaphoreCreateMutex();
    if (KUBIK_CHARACTER == CHARACTER_TESS) {
        s_tess = heap_caps_calloc(1, sizeof(*s_tess), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        assert(s_tess);
        tess_sound_reset(s_tess);  // (builds the sine table: without it every sound is silence until the first quiet())
        tess_sound_seed(s_tess, esp_random());
    }
    if (KUBIK_CHARACTER == CHARACTER_PLUSH) load_kit();
}
