// Exercise the local model's public gesture/clock boundary, independent of sound
// or an agent host. The renderer and production bootstrap are tested separately.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../main/tess.h"

static face_t face;
static void fresh(unsigned progress) {
    memset(&face, 0, sizeof face);
    face.character = CHARACTER_TESS;
    face.mode = MODE_IDLE;
    tess_games_restore(&face, progress);
}
static void advance(float seconds) {
    for (int n = 0; n < (int)lroundf(seconds * 100); n++) tess_games_update(&face, .01f);
}
static void tap(float x, float y) { tess_games_event(&face, FEV_TAP, x, y); }
static void invite_echo(void) {
    tap(240, 255); advance(.35f);
    tap(245, 255); advance(.35f);
    tap(240, 260); advance(1.5f);
}
static void three_taps_then_pause_invite(void) {
    fresh(0);
    tap(240,255); advance(.35f); tap(240,255); advance(.35f); tap(240,255);
    assert(!tess_games_active(&face));
    advance(1.5f);
    assert(tess_games_active(&face) && face.tess_games.game == TESS_GAME_ECHO);
    assert(face.tess_games.progress == 0 && face.menu.open == false && face.card_n == 0);
}
static void ordinary_taps_are_not_games(void) {
    fresh(0); tap(240,255); advance(2); assert(!tess_games_active(&face));
    fresh(0);
    for (int i=0;i<6;i++) { tap(240,255); advance(.35f); }
    advance(2); assert(!tess_games_active(&face));
    fresh(0); tap(100,255); advance(.35f); tap(300,255); advance(.35f); tap(100,255);
    advance(2); assert(!tess_games_active(&face));
}
static void invalid_taps_do_not_start_or_leak_from_games(void) {
    const float invalid[][2]={{10000,10000},{-1,255},{480,255},{240,-1},{240,480},{NAN,255},{240,INFINITY}};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        fresh(0);
        for (int n=0;n<3;n++) { tap(invalid[i][0],invalid[i][1]); advance(.35f); }
        advance(2); assert(!tess_games_active(&face));
        assert(tess_games_start(&face,TESS_GAME_ECHO,0));
        advance(2);
        assert(tess_games_event(&face,FEV_TAP,invalid[i][0],invalid[i][1]));
        assert(face.tess_games.phase==TESS_GAME_WAIT && !face.tess_games.step && !face.tess_games.progress);
    }
}
static void leave_without_a_button(void) {
    fresh(0); invite_echo(); advance(16);
    assert(!tess_games_active(&face) && face.tess_games.progress == 0);
    fresh(0); invite_echo();
    tess_games_event(&face,FEV_PET,240,255);
    assert(!tess_games_active(&face) && face.tess_games.progress == 0);
}
static void ordinary_pet_cancels_a_pending_invitation(void) {
    fresh(0);
    tap(240,255); advance(.35f); tap(240,255); advance(.35f); tap(240,255);
    tess_games_event(&face,FEV_PET,240,255); advance(2);
    assert(!tess_games_active(&face) && face.tess_games.progress==0);
}
static void touching_interrupts_a_learned_trick(void) {
    fresh(1); advance(40.1f);
    assert(face.tess_games.trick==1);
    face.rub_in.down=true; face.rub_in.x=240; face.rub_in.y=255;
    advance(.1f);
    assert(face.tess_games.trick==0);
}
static void circle_trigger_requires_a_closed_deliberate_path(void) {
    for (int direction=-1;direction<=1;direction+=2) {
        fresh(0);
        for (int i=0;i<=120;i++) {
            float a=direction*i*(6.2831853f/120);
            face.rub_in=(rub_input_t){.down=true,.x=240+110*cosf(a),.y=255+110*sinf(a)};
            tess_games_update(&face,.01f);
            if (i==65) tess_games_event(&face,FEV_PET,face.rub_in.x,face.rub_in.y);
        }
        face.rub_in.down=false; advance(.8f);
        assert(tess_games_active(&face) && face.tess_games.game==TESS_GAME_CATCH);
    }
    fresh(0);
    for (int i=0;i<80;i++) {
        face.rub_in=(rub_input_t){.down=true,.x=140+i*2,.y=255};
        tess_games_update(&face,.01f);
    }
    face.rub_in.down=false; advance(2);
    assert(!tess_games_active(&face));
}
static void a_circle_survives_a_short_render_gap(void) {
    for (int direction=-1;direction<=1;direction+=2) {
        fresh(0);
        for (int i=0;i<=40;i++) {
            float a=direction*i*(6.2831853f/40);
            face.rub_in=(rub_input_t){.down=true,.x=240+110*cosf(a),.y=255+110*sinf(a)};
            if (i>10 && i<17) continue; // raw input continues during a 140ms display stall
            tess_games_update(&face,i==17?.14f:.02f);
        }
        face.rub_in.down=false; advance(.8f);
        assert(tess_games_active(&face) && face.tess_games.game==TESS_GAME_CATCH);
    }
}
static void enter_answer_phase(unsigned game,unsigned tier) {
    assert(tess_games_start(&face,(tess_game_t)game,tier));
    for(int n=0;n<400 && face.tess_games.phase!=TESS_GAME_WAIT;n++) advance(.01f);
    assert(face.tess_games.phase==TESS_GAME_WAIT);
}
static void finish_echo(unsigned tier) {
    static const float gaps[3][3]={{.6f,0,0},{.45f,.75f,0},{.45f,.45f,.9f}};
    enter_answer_phase(TESS_GAME_ECHO,tier);
    tap(240,255);
    for (unsigned i=0;i<=tier;i++) { advance(gaps[tier][i]); tap(240,255); }
    assert(face.tess_games.phase==TESS_GAME_CELEBRATE);
    advance(3.1f);
}
static void discoveries_are_finite_and_restore_growth(void) {
    fresh(0); assert(face.tess_games.form==0);
    finish_echo(0); assert(face.tess_games.progress==1 && face.tess_games.form==1);
    finish_echo(0); assert(face.tess_games.progress==1);
    finish_echo(1); assert(face.tess_games.progress==3 && face.tess_games.form==1);
    finish_echo(2); assert(face.tess_games.progress==7 && face.tess_games.form==2);
    for(unsigned tier=0;tier<3;tier++) {
        enter_answer_phase(TESS_GAME_CATCH,tier);
        for(unsigned i=0;i<tier+3;i++) {
            face.tess_games.hit_valid=true;
            face.tess_games.hit[0]=i%2?320:160; face.tess_games.hit[1]=255;
            tap(face.tess_games.hit[0],255); advance(.7f);
        }
        assert(face.tess_games.phase==TESS_GAME_CELEBRATE);
        advance(3.1f);
    }
    assert(face.tess_games.progress==63 && face.tess_games.form==3);
    fresh(63); assert(face.tess_games.form==3);
    advance(300); assert(face.tess_games.progress==63); // no decay from being left alone
}
static void wrong_or_repeated_input_never_grants_a_discovery(void) {
    fresh(0); enter_answer_phase(TESS_GAME_ECHO,0);
    tap(240,255); advance(.1f); tap(240,255);
    assert(!tess_games_active(&face) && !face.tess_games.progress);
    fresh(0); enter_answer_phase(TESS_GAME_CATCH,0);
    face.tess_games.hit_valid=true; face.tess_games.hit[0]=240; face.tess_games.hit[1]=255;
    tap(240,255);
    for(int i=0;i<4;i++) { advance(.7f); tap(240,255); }
    assert(!face.tess_games.progress && face.tess_games.step==1);
    advance(5); assert(!tess_games_active(&face) && !face.tess_games.progress);
    fresh(0); enter_answer_phase(TESS_GAME_CATCH,0);
    face.tess_games.hit_valid=true; face.tess_games.hit[0]=240; face.tess_games.hit[1]=255;
    tap(400,255); assert(!tess_games_active(&face) && !face.tess_games.progress);
}
static void every_priority_context_cancels_without_resume(void) {
    const face_mode_t modes[]={MODE_BOOT,MODE_LISTENING,MODE_THINKING,MODE_SPEAKING,MODE_SLEEP,MODE_SETUP,MODE_OFFLINE};
    for(unsigned i=0;i<sizeof modes/sizeof*modes;i++) {
        fresh(1); enter_answer_phase(TESS_GAME_ECHO,1); tap(240,255);
        face.mode=modes[i]; advance(.1f);
        assert(!face.tess_games.game && face.tess_games.progress==1);
        face.mode=MODE_IDLE; advance(2); assert(!tess_games_active(&face));
    }
    fresh(1); invite_echo(); tess_games_set_available(&face,false);
    assert(!face.tess_games.game && face.tess_games.progress==1);
    tess_games_set_available(&face,true); advance(2); assert(!tess_games_active(&face));
    fresh(0); face.live_active=true; invite_echo(); assert(!tess_games_active(&face));
    fresh(0); face.character=CHARACTER_PLUSH; invite_echo(); assert(!tess_games_active(&face));
}
int main(void) {
    three_taps_then_pause_invite();
    ordinary_taps_are_not_games();
    invalid_taps_do_not_start_or_leak_from_games();
    leave_without_a_button();
    ordinary_pet_cancels_a_pending_invitation();
    touching_interrupts_a_learned_trick();
    circle_trigger_requires_a_closed_deliberate_path();
    a_circle_survives_a_short_render_gap();
    discoveries_are_finite_and_restore_growth();
    wrong_or_repeated_input_never_grants_a_discovery();
    every_priority_context_cancels_without_resume();
    puts("native Tess games: triggers, six discoveries, restore, mistakes, tricks and preemption ok");
}
