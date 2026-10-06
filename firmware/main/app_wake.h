#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "character.h"
#include "app_voice.h"
#if KUBIK_CHARACTER == 1
void app_wake_init(void);
void app_wake_tick(void);
void app_wake_suspend(void);
void app_wake_transport_busy(bool busy);
void app_wake_event(uint32_t epoch);
void app_wake_end_event(int turn, int reason);
void app_wake_abort(bool tell_server);
void app_wake_recording_stop(void);
uint32_t app_wake_session(void);
bool app_wake_recording(void);
bool app_wake_listening(void);
uint32_t app_wake_max_us(void);
uint8_t app_wake_probability(void);
uint8_t app_wake_peak_probability(void);
uint32_t app_wake_average_us(void);
#else
static inline void app_wake_init(void) {}
static inline void app_wake_tick(void) {}
static inline void app_wake_suspend(void) {}
static inline void app_wake_transport_busy(bool busy) { (void)busy; }
static inline void app_wake_event(uint32_t epoch) { (void)epoch; }
static inline void app_wake_end_event(int turn, int reason) { (void)turn; (void)reason; }
static inline void app_wake_abort(bool tell_server) { (void)tell_server; }
static inline void app_wake_recording_stop(void) { app_voice_recording_stop(); }
static inline uint32_t app_wake_session(void) { return 0; }
static inline bool app_wake_recording(void) { return false; }
static inline bool app_wake_listening(void) { return false; }
static inline uint32_t app_wake_max_us(void) { return 0; }
static inline uint8_t app_wake_probability(void) { return 0; }
static inline uint8_t app_wake_peak_probability(void) { return 0; }
static inline uint32_t app_wake_average_us(void) { return 0; }
#endif
