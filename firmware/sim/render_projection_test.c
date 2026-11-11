// Native S3 projection keeps design layout separate from canvas/output pixels.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/render_internal.h"
#include "../main/render_projection.h"
#include "../main/sprite.c"

static scene_t scene;
static render_state_t state;
static uint16_t canvas[R_H][R_W];
static render_workspace_t workspace;
static uint8_t pack[8192];
static const uint8_t *fetch(uint32_t off, uint32_t len, uint32_t hint, uint32_t hint_len) {
    (void)hint; (void)hint_len; assert(off + len <= sizeof pack); return pack + off;
}
static void capture(int x0, int y0, int x1, int y1, uint16_t *pixels, void *context) {
    (void)context;
    for (int y = y0; y < y1; y++)
        memcpy(&canvas[y][x0], pixels + (y - y0) * (x1 - x0), (size_t)(x1 - x0) * 2);
}
static void render(void) {
    render_frame(&state, &scene, &workspace, false, capture, NULL, NULL);
}
static void equal_axes(int *width, int *height) {
    int left=R_W, right=-1, top=R_H, bottom=-1;
    for (int y=0; y<R_H; y++) for (int x=0; x<R_W; x++) if (canvas[y][x]) {
        if (x<left)left=x; if(x>right)right=x; if(y<top)top=y; if(y>bottom)bottom=y;
    }
    *width=right-left+1; *height=bottom-top+1;
    assert(abs(*width-*height)<=1);
}
static void circle_and_dots(void) {
    scene_begin(&scene, 0);
    prim_t *circle=sc_circle(&scene, UI_CX, UI_CY, 64, 0xFFFFFF, 1);
    sc_glow_dot_q3(&scene, 80*8, 90*8, 4*8, 0xFF0000, 0);
    render_project_scene(&scene);
    assert(circle->cx==ui_canvas(UI_CX*16) && circle->cy==ui_canvas(UI_CY*16));
    assert(circle->a==circle->b && circle->b==circle->r);
    assert(scene.dots[0].x==ui_output(80*8) && scene.dots[0].radius==ui_output(4*8));
    prim_t before=*circle; render_project_scene(&scene); assert(!memcmp(&before,circle,sizeof before));
    scene.dots_n=0; render();
    int width,height; equal_axes(&width,&height);
    assert(width>=2*ui_canvas(64) && width<=2*ui_canvas(64)+5);
}
static void design_overlays_and_source_fields(void) {
    scene_begin(&scene,0);
    static uint8_t qr[29*29];
    prim_t *code=sc_qr(&scene,qr,29,174,UI_CX,UI_CY,0xFFFFFF,1);
    int qx=code->cx,qy=code->cy,nx=code->sub_x,ny=code->sub_y;
    prim_t *text=sc_label(&scene,TXT_ACTION,"Bottom",240,UI_H-25,0,0xFFFFFF,1);
    prim_t *icon=sc_icon(&scene,ICON_MIC,460,UI_H-25,32,0xFFFFFF,1);
    assert(text->by1>UI_OUTPUT_H && icon->bx1>UI_RENDER_W);
    int tx=text->sub_x,ty=text->sub_y,ix=icon->sub_x,iy=icon->sub_y,isz=icon->a;
    render_project_scene(&scene);
    assert(code->sub_x==nx && code->sub_y==ny && nx==97 && ny==137 && code->a==29 && code->b==174);
    assert(code->cx==ui_canvas(qx) && code->cy==ui_canvas(qy));
    assert(text->sub_x==tx && text->sub_y==ty);
    assert(icon->sub_x==ix && icon->sub_y==iy && icon->a==isz);
    assert(text->by1<=R_H-1 && icon->bx1<=R_W-1);
}
static void square_sprite_pack(void) {
    memset(pack,0,sizeof pack); memcpy(pack,"KSPR",4);
    pack[4]=5; pack[6]=1; pack[8]=1; pack[12]=240;
    ((uint16_t *)(pack+HDR))[1]=0xF800;
    sprite_anim_t *anim=(sprite_anim_t *)(pack+HDR+512);
    strcpy(anim->name,"square"); anim->count=1; anim->fps=10;
    frame_t *frame=(frame_t *)(pack+HDR+512+ASZ);
    frame->off=700; frame->len=130+64*67; frame->x0=80; frame->x1=143;
    uint8_t *blob=pack+frame->off; blob[0]=80; blob[1]=144;
    for(int row=0;row<64;row++) {
        uint16_t offset=(uint16_t)(row*67); memcpy(blob+2+row*2,&offset,2);
        uint8_t *data=blob+130+offset; data[0]=1; data[1]=80; data[2]=64;
        memset(data+3,1,64);
    }
    assert(sprite_init(fetch));
}
static void sprite_aspect_and_exact_origin(void) {
    square_sprite_pack(); scene_begin(&scene,0);
    prim_t *image=sc_sprite(&scene,0,0,16,52,1); assert(image);
    render();
    assert(image->sub_x==16 && image->sub_r==52);
    int width,height; equal_axes(&width,&height);
    assert(width>=48 && width<=51 && height>=48 && height<=51);
    sprite_deinit();
}
int main(void) {
    assert(R_W==192 && R_H==224 && UI_OUTPUT_W==368 && UI_OUTPUT_H==448);
    circle_and_dots(); design_overlays_and_source_fields(); sprite_aspect_and_exact_origin();
    puts("S3 native projection: uniform circles/dots/sprite, design overlays, source QR and idempotence passed");
    return 0;
}
