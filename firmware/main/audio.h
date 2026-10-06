// Full-duplex audio: ES7210 microphones in, ES8311 speaker out, 24 kHz mono s16.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "character.h"
#include "tess_cue.h"

#define AUDIO_RATE 24000
#define MIC_FRAME_SAMPLES 960  // 40 ms

// Sound kit (tools/sfx/generate.mjs, stored in the assets partition). Groups with
// several variants pick a random one, never the same twice in a row.
typedef enum {
    SFX_BOOT = 0,
    SFX_LISTEN_START,  // capture opens (hold)
    SFX_LATCH_START,   // capture opens (single press, latched)
    SFX_LISTEN_STOP,   // capture closes, phrase sent
    SFX_THINK,         // soft bubbles while the agent works
    SFX_NOTIFY,
    SFX_ERROR,
    SFX_NOT_HEARD,
    SFX_CONNECT,
    SFX_DISCONNECT,
    SFX_WAKE,
    SFX_TAP,
    SFX_GIGGLE,
    SFX_PET,
    SFX_DIZZY,
    SFX_SURPRISE,
    SFX_VOLUME,  // use audio_sfx_volume()
    SFX_HELLO,
    SFX_SCREEN_ON,
    SFX_SCREEN_OFF,
    SFX_SETUP,        // setup opened: a friendly call to come and scan
    SFX_SETUP_PHONE,  // a phone joined Kubik's network
    SFX_SETUP_WAIT,   // soft sonar ping, repeated while joining the home network
    SFX_SETUP_OK,     // joined: small fanfare before the restart
    SFX_SETUP_FAIL,   // could not join: gentle "uh-oh"
    SFX_MENU_OPEN,    // settings panel slides in
    SFX_MENU_CLOSE,   // ... and folds away
    SFX_DETENT,       // volume slider notch: use audio_sfx_level(), 0..10 = 0..100 %
    SFX_GLINT,        // brightness slider notch: likewise
    SFX_ARM,          // "sure?": a dangerous action waits for a second tap
    SFX_DISARM,       // ... and was let go
    SFX_DENY,         // nothing to do here right now (offline, not confirmed)
    SFX_PAGE,         // text card: next page
    SFX_DISMISS,      // something put away: card closed, reply stopped
    SFX_POWER_OFF,    // goodnight
    SFX_COUNT
} sfx_t;

// Active capture supplies its original IMA bytes; local wake capture supplies NULL.
typedef void (*mic_frame_fn)(const int16_t *pcm, int samples, const uint8_t *ima);

void audio_init(int volume);
void audio_tess_gesture(float x, float y);
// A cue from Tess's behaviour (tess_cue.h): a tiny sound of the same family as the sound kit. `strength` 0..1 and
// `position` -1..1 as the cue says. Silent unless the character is Tess; often silent even then (rate limits).
void audio_tess_cue(tess_cue_t cue, float strength, float position);
void audio_set_volume(int pct); // agent speech only
void audio_set_ui_volume(int pct); // interface and character sounds

// One capture task: manual/automatic recording or local Tess wake recognition.
bool audio_mic_gate(bool open, mic_frame_fn fn);
// Select only while capture is closed. Live packs the synchronous speaker reference for host AEC.
void audio_mic_set_duplex(bool enabled);
bool audio_mic_start_epoch(mic_frame_fn fn, uint32_t epoch);
void audio_mic_request_stop(void); // nonblocking invalidation; gate(false) drains later
bool audio_mic_request_stop_epoch(uint32_t epoch); // cannot stop a newer capture
bool audio_mic_is_open(void); // recording only; passive wake never drives listening UI
bool audio_wake_gate(bool open, mic_frame_fn fn);
uint32_t audio_capture_epoch(void);
float audio_mic_level(void);
typedef struct {
    bool open;         // PTT requested and not closing
    bool enabled;      // ADC actually powered; matches adc_enabled
    bool rx_enabled;   // I2S receive channel actually enabled
    bool dozing;       // output codec/TX doze reserved; awake again before capture
    bool adc_enabled;  // codec ADC actually powered
    uint32_t reads;    // bounded attempts made to the I2S driver
    uint32_t idle_reads;  // saturating attempts rejected while capture is off
} audio_mic_status_t;
void audio_mic_status(audio_mic_status_t *status);
uint32_t audio_mic_dropped_ms(void); // DMA queue overflow, cumulative across capture sessions

// Speech stream (server replies).
void audio_stream_begin(void);  // drop anything buffered, start prebuffering
size_t audio_stream_write_ima(const uint8_t *frame, size_t bytes);  // one IMA ADPCM frame (ima_adpcm.h)
void audio_stream_end(void);  // no more data will come: play out the tail
void audio_stream_stop(void);  // stop immediately
bool audio_stream_playing(void);  // audible speech right now
bool audio_stream_drained(void);
void audio_stream_gaps(uint32_t *count, uint32_t *ms);  // mid-speech underruns of the current stream
void audio_stream_timing(uint32_t *mix_us, uint32_t *write_us);
// Screen off: after the output/DMA tail drains, the speaker codec/TX doze until new sound or wake.
// Microphone ADC/RX power follows PTT separately.
void audio_doze(bool doze);
void audio_kick(void);  // a sound, speech or the microphone is about to start: end a doze at once
uint32_t audio_stream_played_ms(void);

float audio_spk_level(void);  // level of what is audible now, 0..1

void audio_sfx(sfx_t id);
void audio_sfx_volume(int level);  // 1..5, pitch climbs with the level
void audio_sfx_level(sfx_t id, int index);  // variant `index` (0-based) of a stepped group, never transposed
bool audio_sfx_playing(void);
// True while a sound effect is audible (incl. the I2S queue tail): the
// microphone hears it, so captured frames are muted meanwhile.
bool audio_self_audible(void);
