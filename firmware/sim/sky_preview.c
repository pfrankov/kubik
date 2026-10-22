// Still frames from the production renderer: catalog figures and telescope focus, with no agent/device.
#include "sim.h"
#include "../main/tess_sky_play.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static face_t face;
static tess_sky_t sky;
static scene_t scene;
static void draw(int chart, float fill, bool resolved) {
    memset(&sky,0,sizeof sky);
    face_init(&face); face_set_character(&face,CHARACTER_TESS); face_set_mode(&face,MODE_IDLE);
    face.boot_t=face.t=10; face.tess_games.game=TESS_GAME_SKY; face.tess_games.sky=&sky;
    sky_recipe_t recipe={.elements=(uint8_t)chart};
    sky_compose(&sky.w,4,&recipe); assert(sky.w.turn==0); // fixed north-up catalog comparison
    sky.w.t=6; sky.phase=resolved ? SKY_RESOLVE : SKY_PLAY;
    sky.resolve_t=3.4f; sky.resolve_len=6; sky.hint_star=-1;
    for(int i=0;i<SKY_PARTICLES;i++)sky.p[i].wait=1;
    for(int i=0;i<sky.w.count_stars;i++)sky.w.star[i].fill=fill;
    sky.melody.n=sky.w.count_stars; sky.melody.unit_ms=160;
    for(int i=0;i<sky.w.count_stars;i++)sky.melody.note[i]=(sky_note_t){.star=(int8_t)i,.units=1};
    scene_begin(&scene,0); tess_sky_draw(&face,&scene); sim_render_scene(&scene); sim_emit_frame();
}
static void stream_frames(void) {
    face.tess_games.sky=NULL;
    face_init(&face); face_set_character(&face,CHARACTER_TESS); face_set_mode(&face,MODE_IDLE);
    tess_games_restore(&face,TESS_PROGRESS_MASK);
    for(int i=0;i<240;i++)face_update(&face,1.f/30);
    face.tess_games.sky_elements=8;face.tess_games.self_wait=999;
    assert(tess_sky_start(&face));
    for(int i=0;i<270;i++) {
        tess_sky_t *k=face.tess_games.sky;
        if(k->phase==SKY_PLAY) {
            sky_force_t force;sky_solution(&k->w,&force);
            face.grav_x=force.gx/SKY_TILT_SCALE;face.grav_y=force.gy/SKY_TILT_SCALE;
            face.view_q[0]=.9238795f;face.view_q[1]=.3826834f;
            face.touch_in.point[0]=(touch_point_t){.x=force.touch[0].x,.y=force.touch[0].y,.down=true};
        }
        face_update(&face,1.f/30);
        tess_tone_t tone;while(face_take_tone(&face,&tone)){}
        tess_cue_t cue;float a,b;while(face_take_cue(&face,&cue,&a,&b)){}
        if(i%90==89) {scene_begin(&scene,0);tess_draw(&face,&scene);sim_render_scene(&scene);sim_emit_frame();}
    }
    tess_games_set_available(&face,false);
}
int main(void) {
    for(int chart=1;chart<=SKY_CHARTS;chart++)draw(chart,1,true);
    for(int i=0;i<=4;i++)draw(1,SKY_FULL*i*.25f,false);
    for(int i=0;i<=4;i++)draw(8,SKY_FULL*i*.25f,false);
    stream_frames();
    return 0;
}
