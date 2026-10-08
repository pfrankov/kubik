// Compile the production eligibility and startup helpers with small app-state stubs.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "app_state.h"
#include "character.h"

static bool s_online, s_setup, s_menu, lab, live, playing, mic_open;
static bool listening, mic_played;
static talk_state_t s_talk;
static power_state_t s_power;
static int s_srv, s_gen, s_act_own;
static char s_pair_code[17];
enum { SS_IDLE, SS_TRANSCRIBING, SS_THINKING };

static bool app_lab_active(void) { return lab; }
static bool app_voice_live_active(void) { return live; }
static bool audio_stream_playing(void) { return playing; }
static unsigned audio_stream_played_ms(void) { return mic_played ? 1u : 0u; }
static bool audio_mic_is_open(void) { return mic_open; }
bool talk_is_listening(talk_state_t state) { return listening || state == TALK_HOLD || state == TALK_LATCHED; }
bool power_is_active(power_state_t state) { return state == PWR_AWAKE || state == PWR_DIMMED; }
bool app_games_flush_safe(void);

/* PRODUCTION_GAMES_AVAILABLE */

typedef struct { unsigned init_calls; uint8_t progress; } face_t;
typedef struct { uint8_t tess_progress; } settings_t;
static face_t g_face;
static settings_t g_settings;
static unsigned sequence;
static unsigned initialized_at, restored_at;

static void face_init(face_t *face) { face->init_calls++; initialized_at = ++sequence; }
static void tess_games_restore(face_t *face, uint8_t progress) {
    assert(face->init_calls);
    face->progress = progress;
    restored_at = ++sequence;
}

/* PRODUCTION_HOME_FACE_INIT */

static void reset_availability(void) {
    s_online = true; s_setup = s_menu = lab = live = playing = mic_open = listening = mic_played = false;
    s_pair_code[0] = 0;
    s_talk = TALK_IDLE; s_power = PWR_AWAKE; s_srv = SS_IDLE; s_gen = -1; s_act_own = 0;
}

static void test_game_availability_is_live_and_preemptible(void) {
    reset_availability(); assert(app_games_available());
    s_online = false; assert(!app_games_available()); s_online = true;
    s_power = PWR_DARK_MANUAL; assert(!app_games_available()); s_power = PWR_DIMMED;
    s_setup = true; assert(!app_games_available()); s_setup = false;
    s_pair_code[0] = '1'; assert(!app_games_available()); s_pair_code[0] = 0;
    s_menu = true; assert(!app_games_available()); s_menu = false;
    lab = true; assert(!app_games_available()); lab = false;
    s_talk = TALK_HOLD; assert(!app_games_available()); s_talk = TALK_IDLE;
    listening = true; assert(!app_games_available()); listening = false;
    live = true; assert(!app_games_available()); live = false;
    s_talk = TALK_AWAITING; assert(!app_games_available()); s_talk = TALK_IDLE;
    s_srv = SS_TRANSCRIBING; assert(!app_games_available()); s_srv = SS_IDLE;
    s_srv = SS_THINKING; assert(!app_games_available()); s_srv = SS_IDLE;
    s_gen = 0; assert(!app_games_available()); s_gen = -1;
    s_act_own = 1; assert(!app_games_available()); s_act_own = 0;
    playing = true; assert(!app_games_available()); playing = false;
    s_gen = 0; mic_played = true; assert(!app_games_available());
    s_gen = -1; mic_played = false; mic_open = true; assert(!app_games_available()); mic_open = false;
}

static void test_flush_waits_for_live_audio_and_agent_reply(void) {
    reset_availability(); assert(app_games_flush_safe());
    mic_open = true; assert(!app_games_flush_safe()); mic_open = false;
    s_talk = TALK_HOLD; assert(!app_games_flush_safe()); s_talk = TALK_IDLE;
    live = true; assert(!app_games_flush_safe()); live = false;
    s_talk = TALK_AWAITING; assert(!app_games_flush_safe()); s_talk = TALK_IDLE;
    s_srv = SS_THINKING; assert(!app_games_flush_safe()); s_srv = SS_IDLE;
    s_gen = 1; assert(!app_games_flush_safe()); s_gen = -1;
    s_act_own = 1; assert(!app_games_flush_safe()); s_act_own = 0;
    playing = true; assert(!app_games_flush_safe());
}

static void test_startup_restores_after_face_initialization(void) {
    g_face = (face_t){0}; g_settings.tess_progress = 0x2d;
    sequence = initialized_at = restored_at = 0;
    initialize_home_face();
    assert(g_face.init_calls == 1 && initialized_at == 1);
    if (KUBIK_CHARACTER == CHARACTER_TESS)
        assert(g_face.progress == 0x2d && restored_at == 2 && restored_at > initialized_at);
    else assert(g_face.progress == 0 && restored_at == 0);
}

int main(void) {
    if (KUBIK_CHARACTER == CHARACTER_TESS) test_game_availability_is_live_and_preemptible();
    else { reset_availability(); assert(!app_games_available()); }
    test_flush_waits_for_live_audio_and_agent_reply();
    test_startup_restores_after_face_initialization();
    puts("Tess app runtime: live availability gates and pre-frame progress restore passed");
}
