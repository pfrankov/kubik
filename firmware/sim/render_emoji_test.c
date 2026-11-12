#include "render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
static scene_t scene,plain;
static unsigned allocation_calls,free_calls;
void *emoji_test_malloc(size_t bytes){allocation_calls++;return malloc(bytes);}
void emoji_test_free(void *data){if(data)free_calls++;free(data);}
static render_emoji_snapshot_t atlas;
static uint8_t atlas_pixels[R_EMOJI_SLOTS][R_EMOJI_BYTES];
static void publish_atlas(void) {
    render_emoji_snapshot_release(&atlas);
    for(unsigned i=0;i<R_EMOJI_SLOTS;i++)
        if(atlas.ready[i]==1){atlas.entries[i]=render_emoji_entry_create(atlas_pixels[i]);assert(atlas.entries[i]);}
}

static uint16_t whole[UI_OUTPUT_H][UI_OUTPUT_W],swapped[UI_OUTPUT_H][UI_OUTPUT_W];
static void prepare(unsigned alpha) {
    scene_end(&scene);render_emoji_snapshot_release(&atlas);
    memset(&atlas,0,sizeof atlas);memset(atlas_pixels,0,sizeof atlas_pixels);strcpy(atlas.keys[0],"😀");atlas.ready[0]=1;
    uint8_t *data=atlas_pixels[0];data[4]=255;data[7]=(uint8_t)alpha;
    data[9]=255;data[11]=(uint8_t)alpha;memset(data+64,0x11,512);
    publish_atlas();scene_begin(&scene,0);scene_emoji_snapshot(&scene,&atlas);
}
static prim_t *label(const char *text,float opacity) {
    return sc_rich_label(&scene,TXT_CARD,text,64,64,-1,0xffffff,opacity);
}
static uint32_t signature(void) {
    int a,b,c,d;return render_text_signature(&scene,&a,&b,&c,&d);
}
static void fill(uint16_t *pixels,size_t count,uint16_t color) {
    for(size_t i=0;i<count;i++)pixels[i]=color;
}
static void normal_and_clusters(void) {
    scene_begin(&scene,0);scene_begin(&plain,0);
    sc_label(&plain,TXT_CARD,"AV Кубик",64,64,-1,0xffffff,.8f);label("AV Кубик",.8f);
    assert(scene.n==1 && scene.text_n==plain.text_n);
    assert(!memcmp(scene.p,plain.p,sizeof(prim_t)));
    assert(!memcmp(scene.text,plain.text,(size_t)scene.text_n*sizeof scene.text[0]));
    assert(rich_label_width(TXT_CARD,"AV Кубик")==label_width(TXT_CARD,"AV Кубик"));
    scene_begin(&scene,0);strcpy(scene.emoji.keys[0],"👩");label("👩‍🚀",1);
    assert(scene.n==1 && scene.text_n==1 && scene.text[0]==R_EMOJI_PLACEHOLDER);
    assert(rich_label_width(TXT_CARD,"👩‍🚀")==32);
    assert(!allocation_calls && !render_emoji_entries_live());
}
static void owned_scene_clone(void) {
    prepare(255);prim_t *p=label("😀",1);int top=p->sub_y-(g_fonts[p->r].cap+32)/2;
    render_emoji_snapshot_t borrowed=scene.emoji;render_emoji_snapshot_release(&atlas);
    scene_emoji_snapshot(&scene,&borrowed); // Existing scene is the sole owner of this borrowed view.
    scene_t held;scene_clone(&held,&scene);scene_end(&scene);render_emoji_snapshot_release(&atlas);
    assert(render_emoji_entries_live()==1);
    fill(&whole[0][0],UI_OUTPUT_H*UI_OUTPUT_W,0);
    render_text_overlay(&held,&whole[0][0],0,0,UI_OUTPUT_W,UI_OUTPUT_H,UI_OUTPUT_W,false);
    assert(whole[ui_output(top+16)][ui_output(80)]==0xf800);
    scene_end(&held);assert(!render_emoji_entries_live());
}
static void snapshot_and_signatures(void) {
    prepare(255);prim_t *p=label("A😀B",1);assert(p && scene.n==1 && scene.text_n==3);
    assert(scene.text[1]==R_EMOJI_TOKEN);
    render_overlay_item_t before,after;assert(render_overlay_item(&scene,p,&before));
    uint32_t ready=signature();atlas.ready[0]=0;publish_atlas();scene_emoji_snapshot(&scene,&atlas);assert(signature()!=ready);
    assert(render_overlay_item(&scene,p,&after));assert(before.signature!=after.signature);
    assert(before.x0==after.x0 && before.y0==after.y0 && before.x1==after.x1 && before.y1==after.y1);
    uint32_t pending=signature();atlas.ready[0]=2;publish_atlas();scene_emoji_snapshot(&scene,&atlas);assert(signature()!=pending);
    prepare(255);label("😀",1);uint32_t original=signature();
    atlas_pixels[0][4]=0;atlas_pixels[0][6]=255;
    assert(render_emoji_entry_pixels(scene.emoji.entries[0])[4]==255 && signature()==original);
    publish_atlas();scene_emoji_snapshot(&scene,&atlas);assert(signature()!=original);
}
static void transparent_colors_do_not_bleed(void) {
    prepare(255);atlas_pixels[0][14]=255; // Blue in a fully transparent straight-alpha palette entry.
    for(unsigned y=0;y<32;y++)for(unsigned x=8;x<16;x++)atlas_pixels[0][64+y*16+x]=0x33;
    publish_atlas();scene_emoji_snapshot(&scene,&atlas);label("😀",1);
    fill(&whole[0][0],UI_OUTPUT_H*UI_OUTPUT_W,0);
    render_text_overlay(&scene,&whole[0][0],0,0,UI_OUTPUT_W,UI_OUTPUT_H,UI_OUTPUT_W,false);
    int x0=UI_OUTPUT_W,y0=UI_OUTPUT_H,x1=0,y1=0;
    for(int y=0;y<UI_OUTPUT_H;y++)for(int x=0;x<UI_OUTPUT_W;x++) {
        uint16_t pixel=whole[y][x];assert(!(pixel&0x07ff));
        if(!pixel)continue;
        if(x<x0) { x0=x; }
        if(x>x1) { x1=x; }
        if(y<y0) { y0=y; }
        if(y>y1) { y1=y; }
    }
    assert(x1>x0 && y1>y0); // Exactly one half-width red box survives interpolation.
    assert(abs(2*(x1-x0+1)-(y1-y0+1))<=2);
}
static void color_alpha_and_order(void) {
    prepare(128);prim_t *p=label("😀",.5f);assert(p);
    fill(&whole[0][0],UI_OUTPUT_H*UI_OUTPUT_W,0x3277);
    fill(&swapped[0][0],UI_OUTPUT_H*UI_OUTPUT_W,rgb565_bswap(0x3277));
    render_text_overlay(&scene,&whole[0][0],0,0,UI_OUTPUT_W,UI_OUTPUT_H,UI_OUTPUT_W,false);
    render_text_overlay(&scene,&swapped[0][0],0,0,UI_OUTPUT_W,UI_OUTPUT_H,UI_OUTPUT_W,true);
    unsigned alpha=(128u*p->alpha*32u+32512u)/65025u;
    int top=p->sub_y-(g_fonts[p->r].cap+32)/2;
    int x=ui_output(80),y=ui_output(top+16);
    assert(whole[y][x]==rgb565_blend(0xf800,0x3277,alpha));
    for(int yy=0;yy<UI_OUTPUT_H;yy++)for(int xx=0;xx<UI_OUTPUT_W;xx++)
        assert(swapped[yy][xx]==rgb565_bswap(whole[yy][xx]));
    atlas_pixels[0][7]=0;publish_atlas();scene_emoji_snapshot(&scene,&atlas);
    fill(&whole[0][0],UI_OUTPUT_H*UI_OUTPUT_W,0x3277);
    render_text_overlay(&scene,&whole[0][0],0,0,UI_OUTPUT_W,UI_OUTPUT_H,UI_OUTPUT_W,false);
    assert(whole[y][x]==0x3277);
}
static void high_nibble_and_tiles(void) {
    prepare(255);atlas_pixels[0][64+16*16]=0x12;publish_atlas();scene_emoji_snapshot(&scene,&atlas);
    prim_t *p=label("😀",1);int top=p->sub_y-(g_fonts[p->r].cap+32)/2;
    fill(&whole[0][0],UI_OUTPUT_H*UI_OUTPUT_W,0);
    render_text_overlay(&scene,&whole[0][0],0,0,UI_OUTPUT_W,UI_OUTPUT_H,UI_OUTPUT_W,false);
#if !CONFIG_IDF_TARGET_ESP32S3
    assert(whole[top+16][64]==0xf800 && whole[top+16][65]==0x07e0);
#else
    (void)top;
#endif
    uint16_t window[11][10];
    for(int y=0;y<UI_OUTPUT_H;y+=11)for(int x=0;x<UI_OUTPUT_W;x+=7) {
        int w=UI_OUTPUT_W-x<7 ? UI_OUTPUT_W-x:7,h=UI_OUTPUT_H-y<11 ? UI_OUTPUT_H-y:11;
        fill(&window[0][0],110,0);for(int yy=0;yy<11;yy++)for(int xx=w;xx<10;xx++)window[yy][xx]=0xa55a;
        render_text_overlay(&scene,&window[0][0],x,y,w,h,10,false);
        for(int yy=0;yy<h;yy++) {
            for(int xx=0;xx<w;xx++)assert(window[yy][xx]==whole[y+yy][x+xx]);
            for(int xx=w;xx<10;xx++)assert(window[yy][xx]==0xa55a);
        }
    }
}
int main(void) {
    normal_and_clusters();snapshot_and_signatures();color_alpha_and_order();high_nibble_and_tiles();
    transparent_colors_do_not_bleed();owned_scene_clone();scene_end(&scene);render_emoji_snapshot_release(&atlas);
    assert(!render_emoji_entries_live() && allocation_calls==free_calls);
    printf("render emoji: native %dx%d colors/alpha/endian/clipping/snapshot/signature/one-primitive passed\n",UI_OUTPUT_W,UI_OUTPUT_H);
}
