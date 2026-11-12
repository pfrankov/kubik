#include "render_internal.h"
#include "emoji_cluster.h"
#include <math.h>
#include <string.h>

int render_token_advance(const font_t *font, uint16_t token) {
    return token>=R_EMOJI_TOKEN ? R_EMOJI_EDGE*16 : font->g[token].adv;
}
int render_token_kern(const font_t *font, uint16_t first, uint16_t second) {
    return first<R_EMOJI_TOKEN && second<R_EMOJI_TOKEN ? font_kern(font,first,second) : 0;
}

void scene_emoji_snapshot(scene_t *scene, const render_emoji_snapshot_t *snapshot) {
    if (!snapshot || snapshot==&scene->emoji) return;
    // Pin before retiring the old frame, including a borrowed view of its slots.
    for(unsigned slot=0;slot<R_EMOJI_SLOTS;slot++)render_emoji_entry_retain(snapshot->entries[slot]);
    render_emoji_snapshot_release(&scene->emoji);
    memcpy(&scene->emoji,snapshot,sizeof *snapshot);
    for(unsigned slot=0;slot<R_EMOJI_SLOTS;slot++) {
        uint32_t hash=(2166136261u^snapshot->ready[slot])*16777619u;
        if(snapshot->ready[slot]==1)hash=(hash^render_emoji_entry_hash(snapshot->entries[slot]))*16777619u;
        scene->emoji_hash[slot]=hash;
    }
}
void scene_end(scene_t *scene) {
    render_emoji_snapshot_release(&scene->emoji);
#ifdef ESP_PLATFORM
    printer_preview_release(scene->printer_preview);
    scene->printer_preview = NULL;
#endif
}
void scene_clone(scene_t *out,const scene_t *source) {
    if(out==source)return;
    memcpy(out,source,sizeof *out);
#ifdef ESP_PLATFORM
    printer_preview_retain(out->printer_preview);
#endif
    for(unsigned i=0;i<R_EMOJI_SLOTS;i++)render_emoji_entry_retain(out->emoji.entries[i]);
}

static uint16_t rich_token(const scene_t *scene, const font_t *font, const char *text, size_t bytes, bool emoji) {
    if(!emoji) return (uint16_t)font_glyph(font,font_utf8_next(&text));
    if(scene && bytes<=64)
        for(unsigned i=0;i<R_EMOJI_SLOTS;i++)
            if(scene->emoji.keys[i][bytes]==0 && !memcmp(scene->emoji.keys[i],text,bytes))return R_EMOJI_TOKEN+i;
    return R_EMOJI_PLACEHOLDER;
}

float rich_label_width(text_role_t role, const char *text) {
    if(role<0 || role>=TXT_ROLES || !text)return 0;
    const text_style_t *style=&g_text[role];const font_t *font=&g_fonts[style->font];
    size_t remaining=strlen(text);int width=0,previous=-1,track=(int)lrintf(style->track*16);
    while(remaining) {
        bool emoji;size_t bytes=emoji_cluster_next(text,remaining,&emoji);
        uint16_t token=rich_token(NULL,font,text,bytes,emoji);
        width+=render_token_advance(font,token);
        if(previous>=0)width+=track+render_token_kern(font,(uint16_t)previous,token);
        previous=token;text+=bytes;remaining-=bytes;
    }
    return width/16.f;
}

prim_t *sc_rich_label(scene_t *scene, text_role_t role, const char *text, float x, float cy,
                      int align, uint32_t rgb, float alpha) {
    if(role<0 || role>=TXT_ROLES || !text || !*text || alpha<=.004f || scene->n>=R_MAX_PRIMS)return NULL;
    const text_style_t *style=&g_text[role];const font_t *font=&g_fonts[style->font];
    int start=scene->text_n;size_t remaining=strlen(text);
    while(remaining && scene->text_n<R_TEXT_POOL) {
        bool emoji;size_t bytes=emoji_cluster_next(text,remaining,&emoji);
        scene->text[scene->text_n++]=rich_token(scene,font,text,bytes,emoji);
        text+=bytes;remaining-=bytes;
    }
    int count=scene->text_n-start;if(!count)return NULL;
    return render_make_text(scene,font,style->font,start,count,x,cy+font->cap*.5f,align,style->track,rgb,alpha);
}

typedef struct {uint8_t r,g,b,a;} rgba_t;
static rgba_t placeholder(int x, int y) {
    bool border=x>=2 && x<30 && y>=2 && y<30 && (x<4 || x>=28 || y<4 || y>=28);
    return (rgba_t){50,50,50,border ? 220 : 0};
}
static rgba_t sample(const uint8_t *data, int x, int y) {
    if(x<0 || y<0 || x>=R_EMOJI_EDGE || y>=R_EMOJI_EDGE)return (rgba_t){0};
    if(!data)return placeholder(x,y);
    unsigned pixel=(unsigned)y*R_EMOJI_EDGE+(unsigned)x;
    unsigned index=(data[64+pixel/2] >> ((pixel&1) ? 0 : 4))&15;
    const uint8_t *color=data+index*4;
    return (rgba_t){color[0],color[1],color[2],color[3]};
}
static int floor_div(int a, int b) {return a>=0 ? a/b : -((-a+b-1)/b);}
static rgba_t filtered(const uint8_t *data, int xq, int yq) {
    int x=floor_div(xq,256),y=floor_div(yq,256),fx=xq-x*256,fy=yq-y*256;
    uint32_t weights[]={ (256u-fx)*(256u-fy),(unsigned)fx*(256u-fy),(256u-fx)*(unsigned)fy,(unsigned)fx*fy };
    rgba_t pixels[]={sample(data,x,y),sample(data,x+1,y),sample(data,x,y+1),sample(data,x+1,y+1)};
    uint32_t a=0,r=0,g=0,b=0;
    for(unsigned i=0;i<4;i++) {
        uint32_t weight=pixels[i].a*weights[i];a+=weight;
        r+=pixels[i].r*weight;g+=pixels[i].g*weight;b+=pixels[i].b*weight;
    }
    if(!a)return (rgba_t){0};
    return (rgba_t){(uint8_t)(r/a),(uint8_t)(g/a),(uint8_t)(b/a),(uint8_t)((a+32768u)>>16)};
}
static void blend(uint16_t *dst, rgba_t source, uint8_t opacity, bool swap) {
    unsigned alpha=(source.a*opacity*32u+32512u)/65025u;
    if(!alpha)return;
    uint16_t color=rgb565((uint32_t)source.r<<16 | (uint32_t)source.g<<8 | source.b);
    uint16_t background=swap ? rgb565_bswap(*dst) : *dst;
    uint16_t result=alpha>=32 ? color : rgb565_blend(color,background,alpha);
    *dst=swap ? rgb565_bswap(result) : result;
}
static int maximum(int a,int b){return a>b ? a:b;}
static int minimum(int a,int b){return a<b ? a:b;}
static int design_axis(int native,int origin) {
    return ((2*native+1)*UI_SCALE_DEN*128)/UI_SCALE_NUM-128-origin*256;
}

void render_emoji_overlay(const scene_t *scene, uint16_t token, uint16_t *out,
    int x0,int y0,int width,int rows,int stride,int left,int top,uint8_t opacity,bool swap) {
    unsigned slot=token-R_EMOJI_TOKEN;
    const uint8_t *data=slot<R_EMOJI_SLOTS && scene->emoji.ready[slot]==1 ? render_emoji_entry_pixels(scene->emoji.entries[slot]) : NULL;
    int xa=maximum(maximum(0,x0),floor_div((left-1)*UI_SCALE_NUM,UI_SCALE_DEN));
    int xb=minimum(minimum(UI_OUTPUT_W,x0+width),-floor_div(-(left+R_EMOJI_EDGE+1)*UI_SCALE_NUM,UI_SCALE_DEN));
    int ya=maximum(maximum(0,y0),floor_div((top-1)*UI_SCALE_NUM,UI_SCALE_DEN));
    int yb=minimum(minimum(UI_OUTPUT_H,y0+rows),-floor_div(-(top+R_EMOJI_EDGE+1)*UI_SCALE_NUM,UI_SCALE_DEN));
    for(int y=ya;y<yb;y++) {
        uint16_t *row=out+(y-y0)*stride;
        int yq=design_axis(y,top);
        for(int x=xa;x<xb;x++)blend(&row[x-x0],filtered(data,design_axis(x,left),yq),opacity,swap);
    }
}
