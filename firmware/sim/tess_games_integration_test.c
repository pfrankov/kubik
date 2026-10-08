#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess_internal.h"

static face_t face;
static scene_t scene;
static void frame(void) { face_update(&face, 1.f / 30); face_draw(&face, &scene); }
static void run(float seconds) { for (int i=0;i<(int)(seconds*30);i++) frame(); }
static void fresh(unsigned progress) {
    face_init(&face);
    face_set_character(&face, CHARACTER_TESS);
    tess_games_restore(&face, progress);
    face_set_mode(&face, MODE_IDLE);
    run(3);
}
static void tap(float x, float y) { face_event(&face, FEV_TAP, x, y); }
static void enter_echo(void) {
    tap(240,255); run(.3f); tap(240,255); run(.3f); tap(240,255); run(1.5f);
    assert(tess_games_active(&face));
}
static void native_events_and_no_extra_sound(void) {
    fresh(0); enter_echo();
    face.tess_play.cue_n = 0;
    for (int i=0;i<180 && face.tess_games.phase!=TESS_GAME_WAIT;i++) frame();
    assert(face.tess_games.phase==TESS_GAME_WAIT);
    tap(240,255); run(.6f); tap(240,255);
    assert(face.tess_games.progress==1);
    assert(face.tess_taps==0 && face.tess_play.cue_n==0);
    run(4);
    assert(face.tess_games.form==1 && !tess_games_active(&face));
}
static void invalid_taps_never_poison_native_touch_state(void) {
    const float invalid[][2]={{NAN,255},{240,INFINITY},{-1,255},{240,480}};
    fresh(0);
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        tap(invalid[i][0],invalid[i][1]);
        assert(isfinite(face.tess_look_to[0]) && isfinite(face.tess_mood.amp) && isfinite(face.tess_touch_x));
        frame();
        assert(!tess_games_active(&face) && !face.tess_games.progress);
    }
}
static void priority_interrupts_before_hidden_simulation(void) {
    fresh(1); enter_echo();
    face.dark=true;
    float at[TESS_N][3];
    memcpy(at,face.tess_position,sizeof at);
    frame();
    assert(!face.tess_games.game && face.tess_games.progress==1);
    assert(memcmp(at,face.tess_position,sizeof at)==0);
    face.dark=false; run(1);
    assert(!tess_games_active(&face));
    enter_echo(); face.menu.open=true; frame();
    assert(!face.tess_games.game);
    face.menu.open=false; run(1);
    enter_echo(); face_card(&face,"An incoming agent reply"); frame();
    assert(!face.tess_games.game && face.card_n>0);
    fresh(0); enter_echo(); run(2);
    tap(240,255);
    face_event(&face, FEV_NOTIFY, 0, 0); // even a notification without visible text preempts the game
    assert(!face.tess_games.game && !face.tess_games.progress);
    run(.6f); tap(240,255);
    assert(!face.tess_games.progress);
}
static void catch_hit_follows_the_rendered_cloud(void) {
    fresh(7);
    assert(tess_games_start(&face,TESS_GAME_CATCH,0));
    run(1.2f);
    assert(face.tess_games.hit_valid);
    for (int caught=0;caught<3;caught++) {
        int budget=180;
        while (caught && (face.tess_games.last_tap<.7f ||
            hypotf(face.tess_games.hit[0]-face.tess_games.last_hit[0],
                   face.tess_games.hit[1]-face.tess_games.last_hit[1])<50) && budget--) frame();
        assert(budget>0);
        tap(face.tess_games.hit[0],face.tess_games.hit[1]);
    }
    assert(face.tess_games.progress==15);
}
int main(void) {
    native_events_and_no_extra_sound();
    invalid_taps_never_poison_native_touch_state();
    priority_interrupts_before_hidden_simulation();
    catch_hit_follows_the_rendered_cloud();
    puts("native Tess integration: events, rendered hits, progress, silent play and priority exits ok");
}
