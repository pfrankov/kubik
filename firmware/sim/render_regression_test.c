// Real assets: the panel, updated band by band where rows changed, must equal a fresh full frame.
// Run from the repository root; use ASan/UBSan when compiling this harness.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/face.h"
#include "../main/tess_internal.h"
#include "../main/tess_points.h"
#include "../main/sprite.h"
#include "../main/canvas.h"
#include "render_band_probe.h"
static unsigned char *pack;
static size_t pack_size;
static const uint8_t *fetch(uint32_t o, uint32_t n, uint32_t ho, uint32_t hl) {
    (void)ho; (void)hl;
    return o <= pack_size && n <= pack_size-o ? pack+o : NULL;
}
typedef struct { uint16_t px[R_H*R_W]; int x0,y0,x1,y1; } test_frame_t;
static test_frame_t full;
static uint16_t panel[480*480], expected[480*480], output[480*4];
static uint16_t bands[2][R_W*R_BAND];
// The device sends a native band once the next one is composed: it expands from the two render bands alone.
static int band_y0[2];
static canvas_t diff;
static render_output_t stream;
static unsigned stream_px, stream_windows, stream_reads, presented_frames;
static void read_band(void *ctx, int y, uint16_t *row, int xa, int xb) {
    (void)ctx; stream_reads++;
    for(int i=0;i<2;i++) if(y>=band_y0[i] && y<band_y0[i]+R_BAND) { memcpy(row,bands[i]+(y-band_y0[i])*R_W,R_W*2); return; }
    abort(); // a row that is no longer (or not yet) held
}
static void expand_band_window(const render_win_t *w) {
    for(int y=w->y0;y<w->y1;y+=4) {
        int n=w->y1-y<4?w->y1-y:4;
        render_expand_2x(&stream,output,w->x0,y,w->x1-w->x0,n,true);
        for(int j=0;j<n;j++) for(int x=w->x0;x<w->x1;x++)
            panel[(y+j)*480+x]=rgb565_bswap(output[j*(w->x1-w->x0)+x-w->x0]);
    }
}
static void send_band(int y0) {
    render_win_t w;
    if(!render_band_window(diff.changed_x0,diff.changed_x1,y0,y0+R_BAND,&w)) return;
    assert(w.x0>=0 && w.x1<=480 && w.y0>=2*y0 && w.y1<=2*(y0+R_BAND) && w.x0<w.x1 && w.y0<w.y1);
    assert(!(w.x0&1) && !(w.x1&1) && !(w.y0&1) && !(w.y1&1));
    render_output_columns(&stream,w.x0,w.x1);
    expand_band_window(&w);
    stream_px+=(unsigned)((w.x1-w.x0)*(w.y1-w.y0)); stream_windows++;
}
static void push_stream(int x0,int y0,int x1,int y1,uint16_t *px,void *ctx) {
    (void)ctx; assert(x0==0 && x1==R_W && y1-y0==R_BAND && px==bands[(y0/R_BAND)&1]);
    band_y0[(y0/R_BAND)&1]=y0;
    assert(canvas_rows(&diff,y0,y1,px));
    if(y0>0) send_band(y0-R_BAND);
}
static void push(int x0,int y0,int x1,int y1,uint16_t *px,void *ctx) {
    test_frame_t *c=ctx;
    assert(x0>=0 && y0>=0 && x1<=R_W && y1<=R_H);
    for(int y=y0;y<y1;y++) memcpy(c->px+y*R_W+x0,px+(y-y0)*(x1-x0),(x1-x0)*2);
}
// Bilinear 2x, written out pixel by pixel: the reference for the device's word-wise expansion.
static uint16_t mid(uint16_t a, uint16_t b) { return (uint16_t)((a & b) + (((a ^ b) & 0xF7DEu) >> 1)); }
static void expand_reference(const uint16_t *px, uint16_t *out) {
    for(int y=0;y<R_H;y++) for(int x=0;x<R_W;x++) {
        int x1=x+1<R_W?x+1:x, y1=y+1<R_H?y+1:y;
        uint16_t a=px[y*R_W+x], ar=mid(a,px[y*R_W+x1]), b=px[y1*R_W+x], br=mid(b,px[y1*R_W+x1]);
        out[2*y*480+2*x]=a; out[2*y*480+2*x+1]=ar;
        out[(2*y+1)*480+2*x]=mid(a,b); out[(2*y+1)*480+2*x+1]=mid(ar,br);
    }
}
// Renders a frame the device's way and checks the panel against a fresh full frame, expanded whole.
static void check_frame(render_state_t *state, const scene_t *scene) {
    scene_t streamed=*scene, reference=*scene;
    uint16_t *bufs[]={bands[0],bands[1]};
    band_y0[0]=band_y0[1]=-R_BAND;
    render_output_begin(&stream,read_band,NULL);
    render_frame(state,&streamed,bufs,2,false,push_stream,NULL);
    send_band(R_H-R_BAND);

    static uint16_t band[R_W*R_BAND]; uint16_t *one[]={band};
    render_state_t reset={0}; render_frame(&reset,&reference,one,1,false,push,&full);
    expand_reference(full.px,expected);
    if(memcmp(panel,expected,sizeof(panel))) {
        static int unused; (void)unused;
        for(int k=0;k<480*480;k++) if(panel[k]!=expected[k]) { fprintf(stderr,"panel mismatch at %d,%d dots %d prims %d got %x expected %x\n",k%480,k/480,scene->dots_n,scene->n,panel[k],expected[k]); break; }
        abort();
    }
    presented_frames++;
}
static face_t face;
static render_state_t state;
static unsigned frames, matrix_frames;
static const face_event_t reactions[]={FEV_TAP,FEV_PET,FEV_SHAKE,FEV_PICKUP,FEV_NOTIFY,FEV_FAIL,FEV_NOT_HEARD};
static void load_assets(void) {
    FILE *fp=fopen("firmware/assets/sprites.bin","rb"); assert(fp);
    fseek(fp,0,SEEK_END); pack_size=ftell(fp); rewind(fp);
    pack=malloc(pack_size); assert(pack); assert(fread(pack,1,pack_size,fp)==pack_size); fclose(fp);
    assert(sprite_init(fetch));
}

static double mean_radius(const face_t *t) {
    double radius=0;
    for(int i=0;i<TESS_N;i++) radius+=sqrt(t->tess_position[i][0]*t->tess_position[i][0]+t->tess_position[i][1]*t->tess_position[i][1]+t->tess_position[i][2]*t->tess_position[i][2]);
    return radius/TESS_N;
}
static bool quarter_turn(float angle) {
    float remainder=fmodf(angle,PI/2); return remainder<2e-3f || PI/2-remainder<2e-3f;
}

static void check_jolt_settles(face_t *a) {
    float rest[TESS_N][3]; memcpy(rest,a->tess_position,sizeof rest);
    a->jolt_dvx=.05f; face_update(a,1.f/R_FPS);
    float moved=0;
    for(int i=0;i<6;i++) face_update(a,1.f/R_FPS);
    for(int i=0;i<TESS_N;i++) moved=fmaxf(moved,fabsf(a->tess_position[i][0]-rest[i][0]));
    assert(moved>.15f);
    assert(a->tess_agitation>0);
    a->tess_next_fidget=99;
    float prev[TESS_N][3]; memcpy(prev,a->tess_position,sizeof prev);
    for(int t=0;t<120;t++) {
        face_update(a,1.f/R_FPS);
        for(int i=0;i<TESS_N;i++) for(int j=0;j<3;j++) {
            assert(fabsf(a->tess_position[i][j]-prev[i][j])<.35f);
            prev[i][j]=a->tess_position[i][j];
        }
    }
    assert(a->tess_agitation<.05f);
}

static void check_dark_resume(face_t *a) {  // a dark panel holds the cloud still; it resumes without a jump
    float held[TESS_N][3]; memcpy(held,a->tess_position,sizeof held);
    a->dark=true; for(int i=0;i<600;i++) face_update(a,.1f);
    assert(!memcmp(held,a->tess_position,sizeof held));
    a->dark=false; face_update(a,.1f);
    for(int i=0;i<TESS_N;i++) for(int j=0;j<3;j++) assert(fabsf(a->tess_position[i][j]-held[i][j])<.35f);
}
static void check_server_emotions(face_t *a) {
    const emotion_t emo[]={EMO_LOVE,EMO_JOY,EMO_SAD,EMO_ANGRY,EMO_CONFUSED,EMO_SHY,EMO_SURPRISED,EMO_DIZZY};
    const tess_reaction_t shows[]={TR_HEART,TR_JOY,TR_SAD,TR_ANGRY,TR_PUZZLED,TR_SHY,TR_SURPRISE,TR_SCATTER};
    for(int e=0;e<8;e++) {
        face_set_emotion(a,emo[e],2.f);
        for(int i=0;i<4;i++) face_update(a,1.f/R_FPS);
        assert(a->tess_reaction[shows[e]]>.2f);
        scene_t sc; face_draw(a,&sc); check_frame(&state,&sc);
    }
}

static void check_tap_reactions(face_t *t) {
    for(int i=0;i<3*R_FPS;i++) face_update(t,1.f/R_FPS);
    int last=-2, repeats=0;
    for(int k=0;k<12;k++) {
        face_event(t, FEV_TAP, 240, 250);
        if(t->tess_last_kick==last) repeats++;
        last=t->tess_last_kick;
        for(int i=0;i<2*R_FPS;i++) face_update(t,1.f/R_FPS);
    }
    assert(repeats==0);
    for(int k=0;k<6;k++) {
        face_event(t, FEV_TAP, 240, 250);
        for(int i=0;i<6;i++) face_update(t,1.f/R_FPS);
    }
    assert(t->tess_hold[TR_SCATTER]>0 || t->tess_reaction[TR_SCATTER]>.1f);
    for(int i=0;i<4*R_FPS;i++) face_update(t,1.f/R_FPS);
}

static void check_sleep_scatter(face_t *t) {
    double awake_r=mean_radius(t);
    face_set_mode(t, MODE_SLEEP);
    double prev=awake_r, jump=0;
    for(int i=0;i<10*R_FPS;i++) {
        face_update(t,1.f/R_FPS);
        double r=mean_radius(t);
        if(fabs(r-prev)>jump) jump=fabs(r-prev);
        prev=r;
    }
    double asleep_r=mean_radius(t);
    fprintf(stderr,"tess r awake %.2f asleep %.2f jump %.3f\n",awake_r,asleep_r,jump);
    assert(asleep_r>awake_r*1.2 && jump<.05);  // (the tesseract itself swells and shrinks by 12 % as it turns)
    face_set_mode(t, MODE_IDLE);
    face_update(t,1.f/R_FPS);
    assert(t->tess_sleep_t<0 && t->tess_wake_t<.1f && t->tess_play.after_what[0]==KICK_HOP);
    for(int i=0;i<2*R_FPS;i++) face_update(t,1.f/R_FPS);
    assert(t->tess_play.after_what[0]==AFTER_NONE);  // the hop has come
    double woke_r=mean_radius(t);
    fprintf(stderr,"tess r woke %.2f\n",woke_r);
    assert(woke_r<awake_r*1.2);
    for(int i=0;i<TESS_N;i++) for(int k=0;k<3;k++) assert(isfinite(t->tess_position[i][k]));
}

static void check_globe_matching(face_t *t) {
    face_set_mode(t, MODE_LISTENING); face_update(t,1.f/R_FPS);
    double way=0;
    for(int i=0;i<TESS_N;i++) {
        const float *g=tess_targets[t->tess_shape_of[i]]; double d=0;
        for(int k=0;k<3;k++) d+=(t->tess_position[i][k]-g[3+k]*1.32f)*(t->tess_position[i][k]-g[3+k]*1.32f);
        way+=sqrt(d);
    }
    way/=TESS_N; fprintf(stderr,"tess: mean way to the globe %.2f\n",way);
    assert(way<.75);
    bool seen[TESS_N]={0};
    for(int i=0;i<TESS_N;i++) { assert(!seen[t->tess_shape_of[i]]); seen[t->tess_shape_of[i]]=1; }
}

static void check_thinking_turn(face_t *t) {
    face_set_mode(t, MODE_THINKING);
    float xw0=t->tess_xw;
    for(int i=0;i<2*R_FPS;i++) face_update(t,1.f/R_FPS);
    assert(t->tess_xw>xw0+1.5f);
    face_set_mode(t, MODE_IDLE);
    for(int i=0;i<4*R_FPS;i++) face_update(t,1.f/R_FPS);
    assert(quarter_turn(t->tess_xw) && quarter_turn(t->tess_zw));
}

static void check_offline_fall(face_t *t) {
    face_set_mode(t, MODE_OFFLINE);
    t->grav_x=0; t->grav_y=1;
    for(int i=0;i<3*R_FPS;i++) face_update(t,1.f/R_FPS);
    assert(t->tess_fallen);
    double my=0, mv=0;
    for(int i=0;i<TESS_N;i++) {
        my+=t->tess_position[i][1]; mv+=fabs(t->tess_velocity[i][0])+fabs(t->tess_velocity[i][1]);
        float pp=4.f/(4.7f-t->tess_position[i][2]), px=240+90*t->tess_position[i][0]*pp, py=255+90*t->tess_position[i][1]*pp;
        assert(px>0 && px<480 && py>0 && py<480);  // the whole screen, not a circle in it
    }
    my/=TESS_N; mv/=TESS_N;
    fprintf(stderr,"tess: fallen mean y %.2f, speed %.3f\n",my,mv);
    assert(my>1.8 && mv<.3);
    t->grav_x=1; t->grav_y=.1f;
    for(int i=0;i<3*R_FPS;i++) face_update(t,1.f/R_FPS);
    double mx=0; for(int i=0;i<TESS_N;i++) mx+=t->tess_position[i][0]; mx/=TESS_N;
    assert(mx>1.5);
    scene_t sc; face_draw(t,&sc); check_frame(&state,&sc);
    face_set_mode(t, MODE_IDLE);
    for(int i=0;i<3*R_FPS;i++) face_update(t,1.f/R_FPS);
    double r=0; for(int i=0;i<TESS_N;i++) r+=fabs(t->tess_position[i][0]); r/=TESS_N;
    assert(!t->tess_fallen && r<1.2);
}

static void check_roll(const scene_t *neutral, const scene_t *roll) {
    double sn=0, cs=0;
    for(int i=0;i<TESS_N;i++) {
        double x=neutral->dots[i].x/8.0-240, y=neutral->dots[i].y/8.0-255, u=roll->dots[i].x/8.0-240, v=roll->dots[i].y/8.0-255;
        cs+=x*x+y*y; sn+=u*u+v*v;
    }
    assert(fabs(sn-cs)/cs<.02);
    double cr=0, dt=0;
    for(int i=0;i<TESS_N;i++) {
        double x=neutral->dots[i].x/8.0-240, y=neutral->dots[i].y/8.0-255, u=roll->dots[i].x/8.0-240, v=roll->dots[i].y/8.0-255;
        cr+=x*v-y*u; dt+=x*u+y*v;
    }
    assert(fabs(atan2(cr,dt)+.2)<.02);
}

static void check_camera_yaw(face_t *view) {
    scene_t yaw;
    float q[4]; memcpy(q,view->view_q,sizeof q);
    for(int k=0;k<3;k++) {
        float h=k==0?.15f:k==1?.75f:1.4f;
        view->view_q[0]=cosf(h); view->view_q[1]=k==2?sinf(h)*.6f:0; view->view_q[2]=sinf(h)*(k==2?.8f:1); view->view_q[3]=0;
        face_draw(view,&yaw);
        double ax=0, ay=0;
        for(int i=0;i<TESS_N;i++) {
            ax+=yaw.dots[i].x/8.0; ay+=yaw.dots[i].y/8.0;
            assert(yaw.dots[i].x>=8*16 && yaw.dots[i].x<232*16 && yaw.dots[i].y>=8*16 && yaw.dots[i].y<232*16);
        }
        assert(fabs(ax/TESS_N-240)<25 && fabs(ay/TESS_N-255)<25);
    }
    memcpy(view->view_q,q,sizeof q);
}

static void check_animation_frames(void) {
    face_init(&face); face.boot_t=5; face_set_mode(&face,MODE_IDLE);
    state=(render_state_t){0}; frames=0;
    for(int a=0;a<BA_COUNT;a++) {
        const sprite_anim_t *an=sprite_anim(face.body.id[a]); assert(an);
        for(int i=0;i<an->count;i++) for(int half=0;half<2;half++) {
            face_update(&face,1.f/30.f);
            face.body.cur=a; face.body.oneshot=true; face.body.pos=i+half*.5f;
            face.emotion=(emotion_t)(a%EMO_COUNT); face_update(&face,0);
            scene_t scene; face_draw(&face,&scene); assert(scene.n < R_MAX_PRIMS);
            check_frame(&state,&scene);
            frames++;
        }
    }
}

static void check_mode_matrix(void) {
    matrix_frames=0;
    for(int mode=MODE_BOOT;mode<=MODE_OFFLINE;mode++) for(int emotion=0;emotion<EMO_COUNT;emotion++) {
        face_init(&face); face.boot_t=5; face_set_mode(&face,(face_mode_t)mode);
        face.emotion=(emotion_t)emotion; face.emotion_left=-1;
        face.battery_low=true; face.charging=true;
        for(int tick=0;tick<24;tick++) {
            face.mic_level=(tick%4)/3.f; face.spk_level=((tick+2)%4)/3.f;
            face.tilt_x=(tick%3)-1; face.tilt_y=((tick+1)%3)-1;
            if(tick==8) face_event(&face,FEV_PET,.5f,.5f);
            if(tick==16) face_event(&face,FEV_VOLUME,.7f,0);
            face_update(&face,1.f/24.f);
            scene_t scene; face_draw(&face,&scene); assert(scene.n < R_MAX_PRIMS);
            check_frame(&state,&scene);
            matrix_frames++;
        }
    }
}

static void check_tess_display(void) {
    // Both characters use the identical streamed-band and display pipeline.
    face_set_character(&face, CHARACTER_TESS);
    body_t sleeping_body = face.body;
    for(int mode=MODE_IDLE; mode<=MODE_OFFLINE; mode++) {
        face_set_mode(&face, mode);
        for(int tick=0; tick<80; tick++) {
            face.mic_level=face.spk_level=(tick%8)/7.f;
            if(tick==20) face_event(&face, FEV_PET, 240, 240);
            face_update(&face,1.f/R_FPS);
            assert(!memcmp(&face.body,&sleeping_body,sizeof sleeping_body));
            scene_t scene; face_draw(&face,&scene);
            assert(scene.dots_n >= 112 && scene.dots_n <= R_MAX_DOTS);
            assert(scene.glass_a < 0 && scene.glass_n == 0); // Tess firmware omits Plush's glass buffers.
            for(int i=0;i<scene.n;i++) assert(scene.p[i].kind != PK_SPRITE);
            check_frame(&state,&scene);
            matrix_frames++;
        }
    }
}

static void check_tess_viewpoint(void) {
    // Camera: the viewpoint orbits around the world-fixed model within the same draw; the model is untouched.
    face_t view; face_init(&view); face_set_character(&view, CHARACTER_TESS); face_set_mode(&view, MODE_IDLE);
    for(int i=0;i<60;i++) face_update(&view,1.f/R_FPS);  // unfolded from the spark
    view.tess_touch_t=2;
    float model[TESS_N][3]; memcpy(model,view.tess_position,sizeof model);
    scene_t neutral, left, right, restored, roll;
    face_draw(&view,&neutral);
    for(int i=0;i<TESS_N;i++) assert(neutral.dots[i].color!=0xFFFF);  // depth colours, never white
    view.view_q[0]=cosf(.3f); view.view_q[2]=sinf(.3f); face_draw(&view,&left);
    view.view_q[2]=-sinf(.3f); view.view_q[1]=.2f; face_draw(&view,&right);
    // Turning the device about the screen normal turns the picture by 0.4 of that angle, the other way.
    view.view_q[0]=cosf(.25f); view.view_q[1]=view.view_q[2]=0; view.view_q[3]=sinf(.25f); face_draw(&view,&roll);
    check_roll(&neutral,&roll);
    check_camera_yaw(&view);
    view.view_q[0]=view.view_q[1]=view.view_q[2]=view.view_q[3]=0;
    assert(memcmp(neutral.dots,left.dots,TESS_N*sizeof(render_dot_t)) && memcmp(left.dots,right.dots,TESS_N*sizeof(render_dot_t)));
    assert(!memcmp(model,view.tess_position,sizeof model));
    for(int j=0;j<TR_COUNT;j++) assert(view.tess_reaction[j]==0);
    face_draw(&view,&restored);
    assert(!memcmp(neutral.dots,restored.dots,TESS_N*sizeof(render_dot_t)));
    puts("Tess viewpoint: 0.4 of the device turn, inverted (incl. roll), centred and on screen, reversible, never white");
}

static void check_tess_life(void) {
    face_t a; face_init(&a); face_set_character(&a, CHARACTER_TESS); face_set_mode(&a, MODE_IDLE);
    float spread0=0, spread1=0;
    face_update(&a,1.f/R_FPS);
    for(int i=0;i<TESS_N;i++) spread0=fmaxf(spread0,fabsf(a.tess_position[i][0]));
    for(int i=0;i<60;i++) face_update(&a,1.f/R_FPS);
    for(int i=0;i<TESS_N;i++) spread1=fmaxf(spread1,fabsf(a.tess_position[i][0]));
    assert(spread0<.2f && spread1>.8f);
    face_set_mode(&a, MODE_LISTENING);
    for(int i=0;i<60;i++) face_update(&a,1.f/R_FPS);
    for(int i=0;i<TESS_N;i++) {
        float r=sqrtf(a.tess_position[i][0]*a.tess_position[i][0]+a.tess_position[i][1]*a.tess_position[i][1]+
                      a.tess_position[i][2]*a.tess_position[i][2]);
        assert(r>1.1f && r<1.55f);
    }
    face_set_mode(&a, MODE_IDLE);
    for(int i=0;i<90;i++) face_update(&a,1.f/R_FPS);
    check_dark_resume(&a);
    check_jolt_settles(&a);
    check_server_emotions(&a);
    puts("Tess life: spark assembly, listening globe, dark resume, jolt and settle, continuous 4D turn, server emotions");
}

static void check_tess_reactions(void) {
    for(int ev=0;ev<7;ev++) {
        face_t morph; face_init(&morph); face_set_character(&morph,CHARACTER_TESS);
        face_set_mode(&morph,MODE_IDLE); morph.tess_touch_t=2;
        for(int i=0;i<60;i++) face_update(&morph,1.f/R_FPS);
        scene_t before, after; face_draw(&morph,&before);
        face_event(&morph,reactions[ev],300,220);face_draw(&morph,&after);
        for(int i=0;i<TESS_N;i++) assert(before.dots[i].x==after.dots[i].x && before.dots[i].y==after.dots[i].y);
        for(int tick=0;tick<150;tick++) {
            face_update(&morph,1.f/R_FPS);face_draw(&morph,&after);
            assert(after.dots_n==TESS_N);
            if(tick%15==0) check_frame(&state,&after);
            for(int j=0;j<TR_COUNT;j++) assert(morph.tess_reaction[j]>=0 && morph.tess_reaction[j]<=1);
        }
        assert(morph.tess_reaction[ev]<.02f);
    }
}

static void check_tess_inertia(void) {
    face_t inertia;face_init(&inertia);face_set_character(&inertia,CHARACTER_TESS);
    face_set_mode(&inertia,MODE_IDLE);face_update(&inertia,.033f);
    face_event(&inertia,FEV_PET,240,240);
    for(int i=0;i<8;i++) face_update(&inertia,.033f);
    float positions[TESS_N][3], velocities[TESS_N][3];
    memcpy(positions,inertia.tess_position,sizeof positions);
    memcpy(velocities,inertia.tess_velocity,sizeof velocities);
    face_event(&inertia,FEV_SHAKE,240,240);face_set_mode(&inertia,MODE_SPEAKING);
    assert(!memcmp(positions,inertia.tess_position,sizeof positions));
    assert(!memcmp(velocities,inertia.tess_velocity,sizeof velocities));
    float moving=0;for(int i=0;i<TESS_N;i++) for(int j=0;j<3;j++) moving+=fabsf(velocities[i][j]);
    assert(moving>1);
    for(int tick=0;tick<300;tick++) {
        if(tick%17==0) face_event(&inertia,reactions[(tick/17)%7],200,300);
        face_update(&inertia,tick%2?.02f:.1f);
        for(int i=0;i<TESS_N;i++) for(int j=0;j<3;j++) {
            assert(isfinite(inertia.tess_position[i][j]) && fabsf(inertia.tess_position[i][j])<4.5f);
            assert(isfinite(inertia.tess_velocity[i][j]) && fabsf(inertia.tess_velocity[i][j])<40);
        }
    }
    puts("Tess inertia: interrupted morph preserves every position/velocity; variable-step springs remain bounded");
    puts("Tess forms: seven continuous bounded point morphs, fixed point count and return verified");
}
static void check_settings(void) {
    float tess_phase=face.tess_phase;
    face_set_character(&face,CHARACTER_PLUSH);
    for(int tick=0; tick<24; tick++) face_update(&face,1.f/R_FPS);
    assert(face.tess_phase==0); // no Tess simulation after unloading
    (void)tess_phase;
    face.menu.open=true; face.menu.k=1;
    body_t hidden_body=face.body;
    for(int tick=0;tick<24;tick++) face_update(&face,1.f/R_FPS);
    assert(!memcmp(&face.body,&hidden_body,sizeof hidden_body));
    int part;
    scene_t menu; face_draw(&face,&menu); assert(menu.n<R_MAX_PRIMS && menu.dots_n==0);
    check_frame(&state,&menu);
    assert(face_menu_hit(&face,72,188,&part)==MENU_VOLUME);
    assert(face_menu_hit(&face,188,80,&part)==MENU_BRIGHT);
    assert(face_menu_hit(&face,356,251,&part)==MENU_GUIDE);
    assert(face_menu_hit(&face,90,428,&part)==MENU_ACTIONS && part==0);
    assert(face_menu_hit(&face,240,428,&part)==-1);  // gap, reset hidden
    assert(face_menu_hit(&face,390,428,&part)==MENU_ACTIONS && part==2);  // power off
    face.menu.service=true;
    assert(face_menu_hit(&face,240,428,&part)==MENU_ACTIONS && part==1);  // unlocked reset
    face_draw(&face,&menu); check_frame(&state,&menu);
    face.menu.service=false;
    assert(face_menu_hit(&face,432,36,&part)==-1); // charge label is not a button
    assert(face_menu_hit(&face,240,472,&part)==-1); // below the last row
    for(int character=0;character<CHARACTER_COUNT;character++) {
        face_t battery; face_init(&battery); face_set_character(&battery,character);
        battery.body.req=-1;
        for(int mode=MODE_IDLE;mode<=MODE_SLEEP;mode++) {
            face_set_mode(&battery,mode);
            scene_t normal, low;
            face_set_power(&battery,true,16,false, false); assert(!battery.battery_low);
            face_draw(&battery,&normal);
            face_set_power(&battery,true,15,false, false); assert(battery.battery_low);
            face_draw(&battery,&low); assert(low.n>normal.n); check_frame(&state,&low);
            face_set_power(&battery,true,0,false, false); assert(battery.battery_low);
            face_set_power(&battery,true,15,true, true); assert(battery.battery_low && battery.charging);
            face_draw(&battery,&low); check_frame(&state,&low);
            face_set_power(&battery,true,-1,false, false); assert(!battery.battery_low);
            face_set_power(&battery,true,101,false, false); assert(!battery.battery_low);
            face_set_power(&battery,false,0,false, false); assert(!battery.battery_low);
        }
    }
    puts("Settings: voice order, compact hit targets, battery threshold/charging/invalid readings and both characters passed");
}

static void check_sleep_pose(void) {
    // Sleeping settles once and remains on the terminal pose; waking takes a pose-linked exit.
    face_init(&face); face.body.req=-1; face_set_mode(&face,MODE_SLEEP);
    for(int tick=0;tick<600;tick++) face_update(&face,1.f/R_FPS);
    assert(face.body.cur==BA_SLEEP);
    const sprite_anim_t *sleep=sprite_anim(face.body.id[BA_SLEEP]);
    for(int tick=0;tick<120;tick++) {
        face_update(&face,1.f/R_FPS);
        assert(face.body.pos==sleep->loop_b && face.body.vel==0);
        scene_t scene; face_draw(&face,&scene); check_frame(&state,&scene);
    }
    face_set_mode(&face,MODE_IDLE);
    for(int tick=0;tick<180;tick++) face_update(&face,1.f/R_FPS);
    assert(face.body.cur!=BA_SLEEP);
}
int main(void) {
    load_assets();
    state=(render_state_t){0};
    check_animation_frames();
    check_mode_matrix();
    check_tess_display(); check_tess_viewpoint(); check_tess_life();
    face_t character; face_init(&character); face_set_character(&character, CHARACTER_TESS); face_set_mode(&character, MODE_IDLE);
    check_tap_reactions(&character); check_sleep_scatter(&character);
    puts("Tess character: varied taps, tap build-up, sleep drifts apart smoothly, waking gathers it back");
    face_t forms; face_init(&forms); face_set_character(&forms, CHARACTER_TESS); face_set_mode(&forms, MODE_IDLE);
    forms.tess_next_fidget=99;
    for(int i=0;i<3*R_FPS;i++) face_update(&forms,1.f/R_FPS);
    assert(quarter_turn(forms.tess_xw) && quarter_turn(forms.tess_zw));
    check_globe_matching(&forms); check_thinking_turn(&forms); check_offline_fall(&forms);
    puts("Tess points: nearest-place matching, conversational 4D turn and quarter-turn rest, offline fall and roll, gathering back");
    check_tess_reactions(); check_tess_inertia(); check_settings(); check_sleep_pose();
    printf("%u mode/emotion/audio/tilt/particle states: streamed-band panel windows match\n",matrix_frames);
    printf("panel: %u px, %u windows, %u rows, %u frames (%.1f px, %.2f wins, %.2f rows/frame)\n",stream_px,stream_windows,stream_reads,presented_frames,(double)stream_px/presented_frames,(double)stream_windows/presented_frames,(double)stream_reads/presented_frames);
    free(pack);
    printf("%u real-asset frames: streamed-band panel windows and byte order match a full frame\n",frames);
    return 0;
}
