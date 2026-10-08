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
    tap(240, 255); advance(.7f); tap(180, 220); advance(.7f);
}
static void ordinary_touch_invites_but_never_awards(void) {
    fresh(0); tap(240,255);
    advance(.7f);
    assert(!tess_games_active(&face) && face.tess_games.invited && face.tess_games.pulse>0);
    tap(180,220);
    assert(tess_games_active(&face) && face.tess_games.step==1);
    assert(face.tess_games.progress==0 && !face.menu.open && !face.card_n);
    fresh(0); tap(240,255); advance(7);
    assert(!tess_games_active(&face) && !face.tess_games.invited && !face.tess_games.progress);
    fresh(0);
    for(int i=0;i<6;i++) { tap(240,255); advance(.25f); }
    advance(2); assert(!tess_games_active(&face) && !face.tess_games.invited);
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
    fresh(0); tap(240,255);
    assert(face.tess_games.pending);
    tess_games_event(&face,FEV_PET,240,255); advance(2);
    assert(!tess_games_active(&face) && !face.tess_games.invited && face.tess_games.progress==0);
    fresh(0); tap(240,255); advance(.7f); assert(face.tess_games.invited);
    tess_games_event(&face,FEV_PET,240,255); tap(240,255);
    assert(!tess_games_active(&face) && !face.tess_games.invited);

}
static void raw_drag_and_hold_cancel_an_idle_invitation(void) {
    for(int hold=0;hold<2;hold++) {
        fresh(0); tap(240,255); advance(.7f); assert(face.tess_games.invited);
        face.rub_in=(rub_input_t){.down=true,.x=240,.y=255}; advance(.1f);
        if(!hold) face.rub_in.y+=30;
        advance(hold?.7f:.1f);
        assert(!face.tess_games.invited && !face.tess_games.pending);
        face.rub_in.down=false; advance(.1f); tap(240,255);
        assert(!tess_games_active(&face));
    }
}
static void touching_interrupts_a_learned_trick(void) {
    fresh(1); advance(40.1f);
    assert(face.tess_games.trick==1);
    face.rub_in.down=true; face.rub_in.x=240; face.rub_in.y=255;
    advance(.1f);
    assert(face.tess_games.trick==0);
}
static void swipe_invites_in_all_directions_and_hold_does_not(void) {
    const int directions[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for(unsigned k=0;k<4;k++) {
        fresh(0);
        for(int i=0;i<=20;i++) {
            face.rub_in=(rub_input_t){.down=true,.x=240+i*5*directions[k][0],.y=255+i*5*directions[k][1]};
            tess_games_update(&face,.01f);
        }
        face.rub_in.down=false; advance(.7f);
        assert(tess_games_active(&face) && face.tess_games.game==TESS_GAME_CATCH);
    }
    fresh(0); face.rub_in=(rub_input_t){.down=true,.x=240,.y=255}; advance(1);
    face.rub_in.down=false; advance(2); assert(!tess_games_active(&face));
    fresh(0); assert(tess_games_start(&face,TESS_GAME_ECHO,0));
    face.rub_in=(rub_input_t){.down=true,.x=240,.y=255}; advance(.1f);
    face.rub_in.x=270; advance(.1f);
    assert(!tess_games_active(&face) && face.tess_games.finger_cancelled);
    face.rub_in.x=350; face.rub_in.down=false; advance(1);
    assert(!tess_games_active(&face)); // a drag exiting play must not immediately start Chase
}
static void enter_answer_phase(unsigned game,unsigned tier) {
    assert(tess_games_start(&face,(tess_game_t)game,tier));
    for(int n=0;n<400 && face.tess_games.phase!=TESS_GAME_WAIT;n++) advance(.01f);
    assert(face.tess_games.phase==TESS_GAME_WAIT);
}
static void finish_echo(unsigned tier) {
    enter_answer_phase(TESS_GAME_ECHO,tier);
    for(unsigned i=0;i<4+2*tier;i++) {
        tap(i%2?180:300, i%3?220:300);
        advance(.65f + .17f*(i%3)); // player pace varies, no exact rhythm to guess
    }
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
    assert(tess_games_active(&face) && !face.tess_games.progress && face.tess_games.step==1);
    fresh(0); enter_answer_phase(TESS_GAME_CATCH,0);
    face.tess_games.hit_valid=true; face.tess_games.hit[0]=240; face.tess_games.hit[1]=255;
    tap(240,255);
    for(int i=0;i<4;i++) { advance(.7f); tap(240,255); }
    assert(!face.tess_games.progress && face.tess_games.step==1);
    advance(5); assert(!tess_games_active(&face) && !face.tess_games.progress);
    fresh(0); enter_answer_phase(TESS_GAME_CATCH,0);
    face.tess_games.hit_valid=true; face.tess_games.hit[0]=240; face.tess_games.hit[1]=255;
    tap(400,255); assert(tess_games_active(&face) && !face.tess_games.progress && !face.tess_games.step);
    assert(face.tess_games.pulse==1 && face.tess_games.destination[0]>0);
}
static void misses_extend_a_round_but_never_its_total_budget(void) {
    fresh(0); enter_answer_phase(TESS_GAME_CATCH,0); advance(5.8f);
    face.tess_games.hit_valid=true; face.tess_games.hit[0]=240; face.tess_games.hit[1]=255;
    tap(400,255); advance(.5f);
    assert(tess_games_active(&face) && face.tess_games.deadline>5 && !face.tess_games.progress);
    for(int i=0;i<40 && tess_games_active(&face);i++) { advance(.6f); tap(10,10); }
    assert(!tess_games_active(&face) && !face.tess_games.progress);
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
    ordinary_touch_invites_but_never_awards();
    invalid_taps_do_not_start_or_leak_from_games();
    leave_without_a_button();
    ordinary_pet_cancels_a_pending_invitation();
    raw_drag_and_hold_cancel_an_idle_invitation();
    touching_interrupts_a_learned_trick();
    swipe_invites_in_all_directions_and_hold_does_not();
    discoveries_are_finite_and_restore_growth();
    wrong_or_repeated_input_never_grants_a_discovery();
    misses_extend_a_round_but_never_its_total_budget();
    every_priority_context_cancels_without_resume();
    puts("native Tess games: invitations, varied duet, forgiving chase, six discoveries and preemption ok");
}
