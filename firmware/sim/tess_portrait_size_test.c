// S3 home body is larger; ordinary motion and voice still fit native output.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/tess_internal.h"
#include "../main/tess_games_internal.h"
#include "../main/face_internal.h"
#include "sim.h"

static face_t face;
static unsigned frames;
static float worst_left=1e6f,worst_right=1e6f,worst_top=1e6f,worst_bottom=1e6f;
#ifdef TESS_SIZE_IMAGES
static const char *directory;
#endif
static void fresh(void) {
    tess_games_set_available(&face,false);
    face_init(&face); face_set_character(&face,CHARACTER_TESS);
    face_mood_off(&face,true); tess_games_restore(&face,TESS_PROGRESS_MASK);
    face_set_mode(&face,MODE_IDLE);
    face.tess_next_fidget=face.next_micro=face.next_saccade=1e6f;
    face.tess_games.self_wait=1e6f;
    for(int i=0;i<240;i++)face_update(&face,1.f/30);
}
static void check_cloud(bool strict) {
    tess_projected_t dots[TESS_N]; tess_project_dots(&face,dots);
    float left=1e6f,right=-1e6f,top=1e6f,bottom=-1e6f;
    for(int i=0;i<TESS_N;i++) {
        float radius=dots[i].radius*3.f/8;
        left=fminf(left,dots[i].x/8.f-radius); right=fmaxf(right,dots[i].x/8.f+radius);
        top=fminf(top,dots[i].y/8.f-radius); bottom=fmaxf(bottom,dots[i].y/8.f+radius);
    }
    if(strict) {
        if (!(left>=0 && right<UI_W && top>=0 && bottom<UI_H))
            fprintf(stderr,"clipping mode%d t%.2f: %.1f..%.1f x %.1f..%.1f\n",face.mode,face.t,left,right,top,bottom);
        assert(left>=0 && right<UI_W && top>=0 && bottom<UI_H);
        worst_left=fminf(worst_left,left); worst_right=fminf(worst_right,UI_W-right);
        worst_top=fminf(worst_top,top); worst_bottom=fminf(worst_bottom,UI_H-bottom);
    }
    frames++;
}
#ifdef TESS_SIZE_IMAGES
static void snapshot(const char *name) {
    if(!directory)return;
    sim_render_reset();sim_render(&face);
    char path[1024];snprintf(path,sizeof path,"%s/%s.ppm",directory,name);
    FILE *out=fopen(path,"wb");assert(out);
    fprintf(out,"P6\n%d %d\n255\n",UI_OUTPUT_W,UI_OUTPUT_H);
    for(int y=0;y<UI_OUTPUT_H;y++)for(int x=0;x<UI_OUTPUT_W;x++) {
        unsigned char rgb[3];sim_to_rgb(panel[y][x],rgb);assert(fwrite(rgb,1,3,out)==3);
    }
    assert(!fclose(out));
}
#else
static void snapshot(const char *name) { (void)name; }
#endif
static void normal_and_tilt(void) {
    fresh();
    tess_projected_t dots[TESS_N];tess_project_dots(&face,dots);
    int left=10000,right=-10000;
    for(int i=0;i<TESS_N;i++){if(dots[i].x<left)left=dots[i].x;if(dots[i].x>right)right=dots[i].x;}
    float native_width=(right-left)/8.f*UI_SCALE_NUM/UI_SCALE_DEN;
    printf("idle native width %.2f\n",native_width);
#if CONFIG_IDF_TARGET_ESP32S3
    assert(native_width>=280 && native_width<=320);
#endif
    snapshot("home");
    for(int i=0;i<360;i++) {
        float angle=.65f*sinf(i*.041f);face.view_q[0]=cosf(angle/2);
        face.view_q[1]=sinf(angle/2);face.view_q[2]=face.view_q[3]=0;
#if CONFIG_IDF_TARGET_ESP32S3
        face.grav_x=.28f*sinf(i*.041f);face.grav_y=.22f*cosf(i*.037f);
#endif
        face_update(&face,1.f/30);check_cloud(true);
    }
    snapshot("tilt");
}
static void rub_and_conversation(void) {
    fresh();
    for(int i=0;i<180;i++) {
        face.rub_in=(rub_input_t){.down=true,.x=240+100*sinf(i*.38f),.y=TESS_CY,.path=35,.turns=(i%12==0)};
        face_update(&face,1.f/30);check_cloud(true);
    }
    assert(face.rub.stage>=RUB_NOTICE);
    face.rub_in.down=false;snapshot("rub");
    const face_mode_t modes[]={MODE_LISTENING,MODE_THINKING,MODE_SPEAKING,MODE_IDLE};
    const char *names[]={"listening","thinking","speaking","return"};
    for(unsigned m=0;m<sizeof modes/sizeof *modes;m++) {
        face_set_mode(&face,modes[m]);
        for(int i=0;i<180;i++){face.spk_level=.6f;face_update(&face,1.f/30);check_cloud(true);}
        snapshot(names[m]);
    }
}
static void extreme_poses(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    fresh();
    const float offsets[][2]={{0,0},{1.45f,0},{-1.45f,0},{0,1.6f},{0,-1.6f}};
    for(unsigned at=0;at<sizeof offsets/sizeof *offsets;at++)for(int i=0;i<96;i++) {
        face.tess_games.offset[0]=offsets[at][0];face.tess_games.offset[1]=offsets[at][1];
        face.tess_xw=i*2*PI/96;face.tess_zw=i*2*PI/73;
        float points[TESS_N][3];tess_point_targets(&face,points,-1);
        memcpy(face.tess_position,points,sizeof points);check_cloud(true);
    }
#endif
}
static void scatter_and_ui(void) {
    fresh();face_event(&face,FEV_SHAKE,0,0);
    for(int i=0;i<30;i++){face_update(&face,1.f/30);check_cloud(false);}
    snapshot("scatter");
    for(int i=0;i<360;i++)face_update(&face,1.f/30);
    check_cloud(true);
    face_card(&face,"A larger Tess keeps the same readable reply card.");
    for(int i=0;i<30;i++)face_update(&face,1.f/30);snapshot("card");
    face.card_n=0;face.card_hold=0;face.menu.open=true;
    for(int i=0;i<30;i++)face_update(&face,1.f/30);snapshot("menu");
    assert(face.menu.k>.99f);
}
int main(int argc,char **argv) {
#ifdef TESS_SIZE_IMAGES
    directory=argc>1?argv[1]:NULL;
#else
    (void)argc; (void)argv;
#endif
    normal_and_tilt();rub_and_conversation();extreme_poses();scatter_and_ui();
    printf("Tess %dx%d: %u checked frames; minimum native halo margins %.1f/%.1f/%.1f/%.1f px\n",
        UI_OUTPUT_W,UI_OUTPUT_H,frames,worst_left*UI_SCALE_NUM/UI_SCALE_DEN,worst_right*UI_SCALE_NUM/UI_SCALE_DEN,
        worst_top*UI_SCALE_NUM/UI_SCALE_DEN,worst_bottom*UI_SCALE_NUM/UI_SCALE_DEN);
    return 0;
}
