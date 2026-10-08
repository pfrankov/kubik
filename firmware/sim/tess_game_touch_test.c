// Actual app tap routing, with physical audio/link services counted rather than
// invoked. The same local game model used by the display handles accepted taps.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tess.h"
#include "app_state.h"
typedef struct { int a,b; unsigned type; } app_ev_t;
enum { EV_TAP, EV_PET };
enum { SFX_PAGE,SFX_DISMISS,SFX_GIGGLE,SFX_TAP,STR_STOPPED,POKE_TAP };
#define ESP_LOGI(...) ((void)0)
static face_t g_face;
static power_state_t s_power;
static talk_state_t s_talk;
static int s_gen, sounds, pokes, cards, stopped, finished, locks, taps, wakes;
static bool s_online, live, stream, card, lab;
static int64_t s_last_touch_or_key;
static int64_t now_ms(void) { return 100; }
static void wake(bool sound) { (void)sound; wakes++; }
static bool app_lab_active(void) { return lab; }
static void audio_tess_gesture(float x,float y) { (void)x; (void)y; }
static bool app_voice_live_active(void) { return live; }
static void app_voice_live_stop(bool tell) { assert(tell); stopped++; live=false; }
static void finish_turn(talk_event_t event) { assert(event==TE_TAP); finished++; }
static void face_lock(void) { assert(!locks); locks++; }
static void face_unlock(void) { assert(locks==1); locks--; }
bool face_card_tap(face_t *f) { assert(f==&g_face); if(card) cards++; return card; }
static bool fallen_character(void) { return false; }
static bool audio_stream_playing(void) { return stream; }
static void audio_sfx(int sound) { (void)sound; sounds++; }
static void stop_speech(bool tell) { assert(tell); stopped++; stream=false; s_gen=-1; }
void face_event(face_t *f,face_event_t event,float x,float y) {
    assert(locks==1); taps++; tess_games_event(f,event,x,y);
}
static void face_ev(face_event_t event,float x,float y) {
    face_lock(); face_event(&g_face,event,x,y); face_unlock();
}
static void bubble(int icon,int text,float seconds) { (void)icon;(void)text;(void)seconds; }
static unsigned esp_random(void) { return 1; }
static void link_poke(int kind) { assert(kind==POKE_TAP); pokes++; }
static bool app_games_available(void) {
    return s_online && power_is_active(s_power) && !live && !stream && s_gen<0 && s_talk==TALK_IDLE;
}
/* PRODUCTION_GAME_TAPS */
/* PRODUCTION_TAP */
static void ready(bool playing) {
    memset(&g_face,0,sizeof g_face); g_face.character=CHARACTER_TESS; g_face.mode=MODE_IDLE;
    tess_games_restore(&g_face,0);
    s_power=PWR_AWAKE; s_talk=TALK_IDLE; s_online=true; s_gen=-1;
    live=stream=card=lab=false; sounds=pokes=cards=stopped=finished=taps=locks=wakes=0;
    if(playing) {
        assert(tess_games_start(&g_face,TESS_GAME_ECHO,0));
        for(int i=0;i<100;i++) tess_games_update(&g_face,.02f);
        assert(g_face.tess_games.phase==TESS_GAME_WAIT);
    }
}
static void game_taps_stay_local(void) {
    const app_ev_t event={.a=240,.b=255,.type=EV_TAP};
    ready(true); touch_tap(&event);
    assert(g_face.tess_games.step==1 && taps==1 && !sounds && !pokes);
    touch_tap(&event); // a tap during the answer is ignored, without an agent poke
    assert(g_face.tess_games.game && g_face.tess_games.step==1 && !g_face.tess_games.progress && !sounds && !pokes);
    ready(false); touch_tap(&event); assert(taps==1 && sounds==1 && pokes==1);
}
static void voice_card_and_link_keep_priority(void) {
    const app_ev_t event={.a=240,.b=255,.type=EV_TAP};
    ready(true); live=true; touch_tap(&event); assert(stopped==1 && !taps && !pokes && !g_face.tess_games.progress);
    ready(true); s_talk=TALK_LATCHED; touch_tap(&event); assert(finished==1 && !taps && !pokes);
    ready(true); card=true; touch_tap(&event); assert(cards==1 && !taps && !pokes);
    ready(true); stream=true; touch_tap(&event);
    assert(stopped==1 && !g_face.tess_games.progress && !g_face.tess_games.game && !pokes);
    ready(true); s_online=false; touch_tap(&event); assert(!g_face.tess_games.game && !pokes);
    assert(!locks);
}
static game_tap_t capture_final_tap(void) {
    const app_ev_t event={.a=240,.b=255,.type=EV_TAP};
    ready(true);
    for(int step=0;step<3;step++) {
        touch_tap(&event);
        for(int i=0;i<35;i++) tess_games_update(&g_face,.02f);
    }
    game_tap_t tap;
    assert(capture_game_tap(&event,&tap));
    wakes=0;
    return tap;
}
static void deferred_taps_are_game_only(void) {
    game_tap_t tap=capture_final_tap();
    apply_game_tap(&tap);
    assert(wakes==1 && !sounds && !pokes);
    for(int i=0;i<30;i++) tess_games_update(&g_face,.02f);
    assert(g_face.tess_games.progress==1);

    tap=capture_final_tap(); g_face.card_n=1; card=true;
    preempt_game(); g_face.card_n=0; card=false; // even if the card is cleared later in this batch
    apply_game_tap(&tap);
    assert(!g_face.tess_games.progress && !g_face.tess_games.game && !cards && !wakes && !pokes);

    tap=capture_final_tap(); s_online=false; preempt_game(); s_online=true;
    apply_game_tap(&tap);
    assert(!g_face.tess_games.progress && !g_face.tess_games.game && !wakes);

    tap=capture_final_tap(); s_power=PWR_DARK_MANUAL; apply_game_tap(&tap);
    assert(!g_face.tess_games.progress && !g_face.tess_games.game && !wakes);
}
static void speech_started_before_dispatch_keeps_priority(void) {
    const app_ev_t event={.a=240,.b=255,.type=EV_TAP};
    game_tap_t tap=capture_final_tap();
    s_gen=42; stream=true; // protocol begins audio before the reserved speak event is handled
    assert(capture_game_tap(&event,&tap));
    apply_game_tap(&tap);
    assert(s_gen==42 && stream && !stopped && !g_face.tess_games.progress && !pokes && !wakes);
}
static void old_taps_cannot_enter_a_replacement_round(void) {
    game_tap_t tap=capture_final_tap();
    assert(tess_games_start(&g_face,TESS_GAME_ECHO,0));
    for(int i=0;i<100;i++) tess_games_update(&g_face,.02f);
    assert(g_face.tess_games.round!=tap.round);
    apply_game_tap(&tap);
    assert(tess_games_active(&g_face) && !g_face.tess_games.step && !wakes);

    uint32_t round=g_face.tess_games.round;
    tess_games_restore(&g_face,0);
    assert(g_face.tess_games.round!=round && g_face.tess_games.round!=tap.round);
    assert(tess_games_start(&g_face,TESS_GAME_ECHO,0));
    for(int i=0;i<100;i++) tess_games_update(&g_face,.02f);
    apply_game_tap(&tap);
    assert(tess_games_active(&g_face) && !g_face.tess_games.step && !wakes);
}
static void capture_keeps_lab_and_ordinary_input_routes(void) {
    const app_ev_t event={.a=240,.b=255,.type=EV_TAP};
    const app_ev_t pet={.a=240,.b=255,.type=EV_PET};
    game_tap_t tap;
    ready(false); assert(!capture_game_tap(&event,&tap));
    ready(true); lab=true; assert(!capture_game_tap(&event,&tap));
    lab=false; assert(!capture_game_tap(&pet,&tap));
    if(KUBIK_CHARACTER==CHARACTER_PLUSH) assert(!capture_game_tap(&event,&tap));
    assert(!wakes && !pokes && !sounds);
}
int main(void) {
    capture_keeps_lab_and_ordinary_input_routes();
    if(KUBIK_CHARACTER==CHARACTER_PLUSH) {
        puts("Plush app taps: native Tess deferral remains disabled");
        return 0;
    }
    game_taps_stay_local();
    voice_card_and_link_keep_priority();
    deferred_taps_are_game_only();
    speech_started_before_dispatch_keeps_priority();
    old_taps_cannot_enter_a_replacement_round();
    puts("Tess app taps: local games, ordinary pokes, voice/card/speech priority and stale-link gate ok");
}
