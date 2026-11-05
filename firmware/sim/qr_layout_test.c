// Real setup layout and renderer: complete edge-to-edge S3 QR patterns and native exports.
#include "../main/face_internal.h"
#include "../main/qr.h"
#include "qrcodegen.h"
#include <assert.h>
#include <stdlib.h>

static uint8_t modules[65 * 65];
static uint16_t canvas[R_H][R_W], panel[UI_OUTPUT_H][UI_RENDER_W];
static scene_t scene;
void draw_wifi(scene_t *s,float x,float y,uint32_t c,float a,float p) {(void)s;(void)x;(void)y;(void)c;(void)a;(void)p;}
static void push(int x0,int y0,int x1,int y1,uint16_t *pixels,void *ctx) {
    (void)ctx;
    for(int y=y0;y<y1;y++)memcpy(&canvas[y][x0],pixels+(y-y0)*(x1-x0),(size_t)(x1-x0)*2);
}
static void read_row(void *ctx,int y,uint16_t *row,int xa,int xb) {
    (void)ctx;memcpy(row+xa,&canvas[y][xa],(size_t)(xb-xa)*2);
}
static void render(void) {
    static uint16_t bands[2][R_W*R_BAND];uint16_t *buffers[]={bands[0],bands[1]};
    render_state_t state={0};render_output_t output;
    memset(canvas,0,sizeof canvas);memset(panel,0,sizeof panel);
    render_frame(&state,&scene,buffers,2,false,push,NULL);
    render_output_begin(&output,read_row,NULL);
    render_expand_2x(&output,&panel[0][0],0,0,UI_RENDER_W,UI_OUTPUT_H,UI_RENDER_W,false);
    render_edge_overlay(&scene,&panel[0][0],0,0,UI_RENDER_W,UI_OUTPUT_H,UI_RENDER_W,false);
    render_text_overlay(&scene,&panel[0][0],0,0,UI_RENDER_W,UI_OUTPUT_H,UI_RENDER_W,false);
}
static void export_frame(const char *out,const char *name,int step) {
    if(!out || !CONFIG_IDF_TARGET_ESP32S3)return;
    char path[1024];snprintf(path,sizeof path,"%s/%s-step%d.rgb",out,name,step);
    FILE *f=fopen(path,"wb");assert(f);
    for(int y=0;y<UI_OUTPUT_H;y++)for(int x=0;x<UI_OUTPUT_W;x++) {
        uint16_t pixel=panel[y][x];uint8_t rgb[]={((pixel>>11)&31)*255/31,((pixel>>5)&63)*255/63,(pixel&31)*255/31};
        assert(fwrite(rgb,1,sizeof rgb,f)==sizeof rgb);
    }
    assert(!fclose(f));
}
static const prim_t *setup(int n,int step) {
    face_t face={0};face.character=CHARACTER_TESS;
    face_set_setup(&face,modules,n,step,step==1 ? "1 · Scan to join Wi-Fi" : "2 · Scan again for setup","192.168.4.1");
    face.qr_t=1;scene_begin(&scene,0);draw_setup_card(&face,&scene);
    const prim_t *qr=NULL;unsigned text=0;
    for(int i=0;i<scene.n;i++) {
        const prim_t *p=&scene.p[i];
        if(p->kind==PK_QR) {assert(!qr);qr=p;}
        text+=p->kind==PK_TEXT;
    }
    assert(qr && text==2 && qr->a==n);
    return qr;
}
#if !CONFIG_IDF_TARGET_ESP32S3
static void original_c6_labels(void) {
    static const int baselines[]={46,447};unsigned seen=0;
    for(int i=0;i<scene.n;i++)if(scene.p[i].kind==PK_TEXT) {
        assert(seen<2 && scene.p[i].sub_y==baselines[seen]);seen++;
    }
    assert(seen==2);
}
static void original_c6_pixels(const prim_t *qr) {
    static const int original[]={8,6,6,5,4,4};
    assert(qr->b==original[(qr->a-21)/4]);
    assert(qr->cx==120-qr->a*qr->b/2 && qr->cy==120-qr->a*qr->b/2);
    for(int y=0;y<qr->a;y++)for(int x=0;x<qr->a;x++) {
        uint16_t want=modules[y*qr->a+x] ? rgb565(0xE6ECEA) : 0;
        assert(panel[(qr->cy+y*qr->b)*2+qr->b][(qr->cx+x*qr->b)*2+qr->b]==want);
    }
}
#endif
static void native_pixels(const prim_t *qr) {
#if CONFIG_IDF_TARGET_ESP32S3
    int n=qr->a,side=qr->b,ox=qr->sub_x,oy=qr->sub_y;
    assert(side==368 && ox==0 && oy==36);
    for(int y=oy;y<oy+side;y++)for(int x=0;x<368;x++) {
        uint16_t want=modules[((y-oy)*n/side)*n+x*n/side] ? rgb565(0xE6ECEA) : 0;
        assert(panel[y][x]==want); // Includes x=0 and x=367: no source cells or edge pixels are cropped.
    }
    for(int x=0;x<368;x++) {
        assert(!panel[oy-2][x] && !panel[oy-1][x]);
        if(panel[oy+side][x] || panel[oy+side+1][x])fprintf(stderr,"QR footer AA enters clearance at x=%d y=%d/%d values=%x/%x\n",x,oy+side,oy+side+1,panel[oy+side][x],panel[oy+side+1][x]);
        assert(!panel[oy+side][x] && !panel[oy+side+1][x]);
    }

#else
    original_c6_pixels(qr);original_c6_labels();
#endif
}
static void check(int n,const char *name,const char *out) {
    int size=0;
    for(int step=1;step<=2;step++) {
        const prim_t *qr=setup(n,step);
        if(step==1)size=qr->b;else assert(qr->b==size);
        render();native_pixels(qr);export_frame(out,name,step);
    }
    printf("QR %s: %d source modules, %s %d px, both steps\n",name,n,CONFIG_IDF_TARGET_ESP32S3 ? "native side" : "original canvas module",size);
}
int main(int argc,char **argv) {
    const char *out=argc>1 ? argv[1] : NULL;
    static uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],temp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    for(int version=1;version<=6;version++) {
        int n=17+4*version;char name[32];snprintf(name,sizeof name,"source-%d",n);
        char payload[32];snprintf(payload,sizeof payload,"KUBIK-QR-%d",n);
        assert(qrcodegen_encodeText(payload,temp,code,qrcodegen_Ecc_MEDIUM,version,version,qrcodegen_Mask_AUTO,true));
        for(int y=0;y<n;y++)for(int x=0;x<n;x++)modules[y*n+x]=qrcodegen_getModule(code,x,y);
        check(n,name,out);
    }
    const char *wifi="WIFI:T:WPA;S:Kubik-4F2A;P:kubik4f2abcd;;";
    int n=qr_make(wifi,modules);assert(n>=21 && n<=QR_MAX_N);check(n,"wifi",out);
#if CONFIG_IDF_TARGET_ESP32S3
    for(n=45;n<=65;n+=4) {
        memset(modules,1,(size_t)n*n);const prim_t *qr=setup(n,1);render();native_pixels(qr);
    }
#endif
    puts(CONFIG_IDF_TARGET_ESP32S3 ? "S3 QR layout: real setup/render path, six SDK sizes, 21..65 bounds and complete edge-to-edge 368px patterns passed" : "C6 QR layout: original six source sizes, both steps and exact canvas geometry preserved");
}
