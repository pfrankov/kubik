#!/usr/bin/env python3
"""Exercise the actual S3 scaler, partial-window halos and delayed DMA ownership."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'firmware/main/display_s3.c').read_text()
source = source.replace('#include "board.h"\n', '').replace('#include "esp_heap_caps.h"\n', '').replace('#include "esp_timer.h"\n', '').replace('#include "display_te_s3.h"\n', '').replace('#include "esp_attr.h"\n', '').replace('#include "esp_log.h"\n', '')
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "display_s3.h"
#include "viewport.h"
#define LCD_PIPE 3
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_SPIRAM 8
static int allocation_call, allocation_fail, held;
static void *heap_caps_malloc(size_t bytes, unsigned caps) {
    ++allocation_call;
    assert(((bytes==480*584*2 || bytes==368*448*2) && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)) ||
           (bytes==368*8*2 && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT)));
    if(allocation_call==allocation_fail) return NULL;
    void *p=malloc(bytes); assert(p); held++; return p;
}
static void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps) {
    void *p=heap_caps_malloc(count*bytes,caps); if(p)memset(p,0,count*bytes); return p;
}
static void heap_caps_free(void *p) { if(p)held--; free(p); }
static uint16_t screen[448][368], logical[592][480];
static bool raw_open, first_of_burst;
static int te_waits;
#define ESP_ERROR_CHECK(value) assert((value)==0)
static void verify_native_ready(void);
static int64_t mock_time=123456,mock_step;
static int64_t esp_timer_get_time(void) {int64_t now=mock_time;mock_time+=mock_step;return now;}
#define IRAM_ATTR
#ifdef ESP_PLATFORM
static int logged;
#define ESP_LOGI(...) (++logged)
#endif
static int raw_x0,raw_x1,raw_y,raw_y1,windows,writes;
struct queued {const uint16_t *data; int count,x,y,width; uint32_t hash;};
static struct queued queue[3];
static int pending;
static uint32_t hash(const uint16_t *p,int n) {uint32_t h=2166136261u;for(int i=0;i<n;i++)h=(h^p[i])*16777619u;return h;}
static void unchanged(void) {for(int i=0;i<pending;i++)assert(hash(queue[i].data,queue[i].count)==queue[i].hash);}
static void complete_one(void) {
    unchanged(); assert(pending);
    struct queued *q=&queue[0];
    for(int row=0;row<q->count/q->width;row++)
        memcpy(&screen[q->y+row][q->x],q->data+row*q->width,(size_t)q->width*2);
    memmove(queue,queue+1,(size_t)(--pending)*sizeof *queue);
}
void board_display_begin(int x0,int y0,int x1,int y1) {
    if(first_of_burst) {verify_native_ready();first_of_burst=false;}
    unchanged(); assert(!raw_open && x0>=0 && x1<=368 && x0<x1 && y0>=0 && y1<=448 && y0<y1);
    raw_open=true;raw_x0=x0;raw_x1=x1;raw_y=y0;raw_y1=y1;windows++;
}
void board_display_write(const uint16_t *data,int count,bool last) {
    unchanged(); assert(raw_open && count>0 && count<=(raw_x1-raw_x0)*8);
    assert(count%(raw_x1-raw_x0)==0 && raw_y+count/(raw_x1-raw_x0)<=raw_y1);
    if(pending==2)complete_one();
    queue[pending++]=(struct queued){data,count,raw_x0,raw_y,raw_x1-raw_x0,hash(data,count)};
    raw_y+=count/(raw_x1-raw_x0); assert(last==(raw_y==raw_y1));writes++;
}
void board_display_end(void) {unchanged();assert(raw_open && raw_y==raw_y1);raw_open=false;}
void board_display_wait_idle(void) {while(pending)complete_one();}
static int display_te_wait(void) {verify_native_ready();board_display_wait_idle();te_waits++;return 0;}
'''
checks = r'''
static uint16_t wire(uint16_t value) {return (uint16_t)((value<<8)|(value>>8));}
static void write_rect(int x0,int y0,int x1,int y1,int rows) {
    display_s3_begin(x0,y0,x1,y1);
    uint16_t data[480*16];
    for(int y=y0;y<y1;y+=rows) {
        int n=y1-y<rows ? y1-y : rows;
        for(int j=0;j<n;j++)memcpy(data+j*(x1-x0),&logical[y+j][x0],(size_t)(x1-x0)*2);
        display_s3_write(data,(x1-x0)*n,y+n==y1);
    }
    display_s3_end();
}
static void draw(int x0,int y0,int x1,int y1,int rows) {
    display_s3_begin_frame();write_rect(x0,y0,x1,y1,rows);
    first_of_burst=true;assert(display_s3_present()==123456);assert(!first_of_burst);
}
static unsigned channel(uint16_t value,int shift,unsigned mask) {return (value>>shift)&mask;}
static void verify(void) {
    board_display_wait_idle();
    for(int y=0;y<448;y++)for(int x=0;x<368;x++) {
        double sx=(x+.5)*480./368.-.5,sy=(y+.5)*584./448.-.5;
        int ax=(int)sx,ay=(int)sy; double fx=sx-ax,fy=sy-ay;
        const int shifts[]={11,5,0}; const unsigned masks[]={31,63,31};
        for(int c=0;c<3;c++) {
            double top=channel(logical[ay][ax],shifts[c],masks[c])*(1-fx)+channel(logical[ay][ax+1],shifts[c],masks[c])*fx;
            double bottom=channel(logical[ay+1][ax],shifts[c],masks[c])*(1-fx)+channel(logical[ay+1][ax+1],shifts[c],masks[c])*fx;
            double wanted=top*(1-fy)+bottom*fy;
            assert(fabs((double)channel(wire(screen[y][x]),shifts[c],masks[c])-wanted)<=1.01);
        }
    }
}
static unsigned scalar(unsigned a,unsigned b,unsigned fraction) {
    return (a*(256-fraction)+b*fraction+128)>>8;
}
static uint16_t reference_mix(uint16_t a,uint16_t b,unsigned f) {
    return (uint16_t)((scalar(a>>11,b>>11,f)<<11)|(scalar((a>>5)&63,(b>>5)&63,f)<<5)|scalar(a&31,b&31,f));
}
static uint16_t reference_pixel(int x,int y) {
    int qx=((2*x+1)*480*256)/(2*368)-128,qy=((2*y+1)*584*256)/(2*448)-128;
    int ax=qx/256,ay=qy/256;unsigned fx=qx%256,fy=qy%256;
    uint16_t top=reference_mix(logical[ay][ax],logical[ay][ax+1],fx);
    uint16_t bottom=reference_mix(logical[ay+1][ax],logical[ay+1][ax+1],fx);
    return wire(reference_mix(top,bottom,fy));
}
static void verify_native_ready(void) {
    // The first SPI command may only occur after all damaged bands, including
    // later windows in this frame, already contain their final filtered pixels.
    for(int band=0;band<56;band++)for(int y=band*8;y<(band+1)*8;y++)
        for(int x=s_left[band];x<s_right[band];x++)assert(s_native[y*368+x]==reference_pixel(x,y));
}
static void exact_mix(void) {
    uint32_t random=0x71ef9;
    for(unsigned i=0;i<1000000;i++) {
        random=random*1664525u+1013904223u;uint16_t a=(uint16_t)random;
        random=random*1664525u+1013904223u;uint16_t b=(uint16_t)random;
        unsigned f=(random>>16)%257;
        uint16_t expected=(uint16_t)((scalar(a>>11,b>>11,f)<<11)|
            (scalar((a>>5)&63,(b>>5)&63,f)<<5)|scalar(a&31,b&31,f));
        assert(mix_rgb565(a,b,f)==expected);
    }
}
static void touch_mapping(void) {
    // The inverse picks the closest logical pixel centre to the display filter's
    // sample position. The rectangular UI covers both full physical axes.
    for(int y=-1;y<=448;y++)for(int x=-1;x<=368;x++) {
        int16_t lx=-7,ly=-9;
        bool inside=x>=0 && x<368 && y>=0 && y<448;
        assert(display_s3_touch(x,y,&lx,&ly)==inside);
        if(!inside) {assert(lx==-7 && ly==-9);continue;}
        int expected_x=(int)((x+.5)*480./368.);
        int expected_y=(int)((y+.5)*584./448.);
        assert(lx==expected_x && ly==expected_y && lx>=0 && lx<480 && ly>=0 && ly<584);
    }
    int16_t x=-1,y=-1;
    assert(display_s3_touch(0,0,&x,&y) && x==0 && y==0);
    assert(display_s3_touch(367,447,&x,&y) && x==479 && y==583);
    assert(display_s3_touch(184,224,&x,&y) && x==240 && y==292);
    assert(!display_s3_touch(0,0,NULL,&y));
    assert(!display_s3_touch(0,0,&x,NULL));
}
int main(void) {
    assert(UI_W==480 && UI_H==584 && UI_RENDER_H==592);
    touch_mapping();exact_mix();
    // Every allocation failure is surfaced; partially created pools are freed.
    for(int fail=1;fail<=5;fail++) {
        allocation_call=0; allocation_fail=fail;
        assert(!display_s3_init() && held==0);
    }
    allocation_call=0; allocation_fail=0;
    assert(display_s3_init() && held==5 && allocation_call==5);
    for(int y=0;y<592;y++)for(int x=0;x<480;x++)
        logical[y][x]=y<584 ? (uint16_t)(((x*31/479)<<11)|((y*63/583)<<5)|((x+y)*31/1062)) : 0xf81f;
    draw(0,0,480,592,1); verify();
    assert(windows==1 && writes==56);
    display_s3_profile_t profile=display_s3_last_profile();
    // One-logical-row chunks update the two distinct source neighbours of
    // every native sample separately: exactly two preparations per pixel.
    // Closing/presenting the window must not prepare it for a third time.
    assert(profile.input_pixels==480*592 && profile.native_pixels==2*368*448 && profile.windows==1);
    assert(profile.copy_us==0 && profile.prepare_us==0 && profile.te_us==0 && profile.burst_us==0);
    int before=windows;
    display_s3_begin_frame();
    logical[1][1]=0xf800;write_rect(1,1,2,2,1);
    assert(windows==before);
    logical[582][478]=0x07e0;write_rect(478,582,479,583,1);
    assert(windows==before);
    first_of_burst=true;assert(display_s3_present()==123456);verify();
    before=windows;int prior_te=te_waits;assert(display_s3_present()==0 && windows==before && te_waits==prior_te);
    // Every copied chunk prepares its pixels even before the window closes.
    display_s3_begin_frame();before=windows;
    display_s3_begin(120,200,124,204);
    uint16_t chunk[8];
    for(int row=0;row<2;row++)for(int x=0;x<4;x++)chunk[row*4+x]=logical[200+row][120+x]=0xf81f;
    display_s3_write(chunk,8,false);assert(windows==before);
    for(int y=0;y<448;y++)for(int x=0;x<368;x++)assert(s_native[y*368+x]==reference_pixel(x,y));
    for(int row=0;row<2;row++)for(int x=0;x<4;x++)chunk[row*4+x]=logical[202+row][120+x]=0x1529;
    display_s3_write(chunk,8,true);assert(windows==before);
    for(int y=0;y<448;y++)for(int x=0;x<368;x++)assert(s_native[y*368+x]==reference_pixel(x,y));
    uint32_t prepared=s_profile.native_pixels;display_s3_end();assert(s_profile.native_pixels==prepared);
    first_of_burst=true;assert(display_s3_present()==123456);verify();
    // Later overlapping windows repair all affected filter neighbours in both
    // axes; time accumulates per chunk and is not counted again at end/present.
    display_s3_begin_frame();before=windows;mock_step=5;
    for(int y=100;y<116;y++)for(int x=100;x<110;x++)logical[y][x]=(x+y)&1 ? 0xf800 : 0x001f;
    write_rect(100,100,110,116,4);assert(windows==before && s_profile.prepare_us==20);verify_native_ready();
    for(int y=108;y<120;y++)for(int x=105;x<113;x++)logical[y][x]=0x7be7;
    write_rect(105,108,113,120,3);assert(windows==before && s_profile.prepare_us==40);verify_native_ready();
    // Separated columns in the same damage band widen its send hull; the gap
    // remains correct without resampling the whole accumulated hull.
    for(int y=111;y<115;y++)logical[y][130]=0x53a9;
    write_rect(130,111,131,115,1);assert(windows==before && s_profile.prepare_us==60);verify_native_ready();
    mock_step=0;mock_time=123456;
    first_of_burst=true;assert(display_s3_present()==123456);verify();
    assert(display_s3_last_profile().prepare_us==60);
    // Each single pixel and thin edge update must repaint every sample that reads
    // it; output is identical to resending the final full logical image.
    uint32_t random=9183;
    for(int n=0;n<48;n++) {
        random=random*1664525u+1013904223u;int x=(int)(random%480);
        random=random*1664525u+1013904223u;int y=(int)(random%584);
        logical[y][x]=(uint16_t)random; draw(x,y,x+1,y+1,1);
    }
    for(int y=0;y<584;y++) {logical[y][0]=0xf800;logical[y][479]=0x001f;}
    draw(0,0,1,584,8);draw(479,0,480,584,8);
    for(int x=0;x<480;x++) {logical[0][x]=0x07e0;logical[583][x]=0xffff;}
    draw(0,0,480,1,1);draw(0,583,480,584,1);verify();
    static uint16_t partial[448][368];memcpy(partial,screen,sizeof screen);
    draw(0,0,480,584,16);board_display_wait_idle();assert(!memcmp(partial,screen,sizeof screen));
    // Render padding never becomes visible or modifies either retained canvas.
    static uint16_t retained[584][480];memcpy(retained,s_frame,sizeof retained);
    memcpy(partial,screen,sizeof screen);before=windows;prior_te=te_waits;
    display_s3_begin_frame();write_rect(0,584,480,592,8);
    assert(display_s3_present()==0 && windows==before && te_waits==prior_te);
    assert(!memcmp(retained,s_frame,sizeof retained) && !memcmp(partial,screen,sizeof screen));
    for(int y=579;y<592;y++)for(int x=0;x<480;x++)logical[y][x]=0x7531;
    draw(0,579,480,592,4);verify();
    // A logical circle remains round when the rectangular UI fills the panel.
    memset(logical,0,sizeof logical);
    for(int y=0;y<584;y++)for(int x=0;x<480;x++)
        if((x-240)*(x-240)+(y-292)*(y-292)<=120*120)logical[y][x]=0xffff;
    draw(0,0,480,592,8);board_display_wait_idle(); // Exact Q8 pixels are checked before TE; here measure circle aspect.
    int left=368,right=-1,top=448,bottom=-1;
    for(int y=0;y<448;y++)for(int x=0;x<368;x++)if(channel(wire(screen[y][x]),0,31)>15) {
        if(x<left)left=x;if(x>right)right=x;if(y<top)top=y;if(y>bottom)bottom=y;
    }
    assert(abs((right-left)-(bottom-top))<=1);
    // Constant host-order colours cover the entire physical rectangle exactly.
    for(int y=0;y<592;y++)for(int x=0;x<480;x++)logical[y][x]=0x95b7;
    draw(0,0,480,592,8);board_display_wait_idle();
    for(int y=0;y<448;y++)for(int x=0;x<368;x++)assert(screen[y][x]==wire(0x95b7));
    assert(display_s3_init() && held==5 && allocation_call==5); // reset reuses storage
    memset(logical,0,sizeof logical);draw(0,0,1,1,1);board_display_wait_idle();
    assert(screen[0][0]==0);release_buffers();assert(held==0);
#ifdef ESP_PLATFORM
    s_last_profile=(display_s3_profile_t){.copy_us=26000};
    for(int i=0;i<8;i++)report_slow_phases();assert(logged==4);
    mock_time+=10000000;report_slow_phases();assert(logged==5);
    s_last_profile.copy_us=25000;report_slow_phases();assert(logged==5);
#endif
    puts("S3 display: full rectangle without shape stretching, RGB565 interpolation, halo updates, delayed DMA ownership, exact Q8 packed colour, touch inverse, reset and allocation failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-display-s3-') as temporary:
    directory = Path(temporary)
    path = directory / 'test.c'
    path.write_text(fixture + source + checks)
    binary = directory / 'test'
    (directory / 'sdkconfig.h').write_text('#define CONFIG_IDF_TARGET_ESP32S3 1\n')
    for flags in ([], ['-DESP_PLATFORM=1']):
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
                        '-fsanitize=address,undefined', '-DCONFIG_IDF_TARGET_ESP32S3=1', '-I', str(directory), '-I', str(ROOT / 'firmware/main'), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

# Compile the actual presentation boundary for each target: expansion and both
# full-resolution overlays must use the same order required by that destination.
presentation = (ROOT / 'firmware/main/present.c').read_text()
send_window = presentation[presentation.index('static void send_window('):presentation.index('static int larger(')]
order_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define LCD_W 480
#define OUTPUT_ROWS 8
#define LCD_PIPE 3
typedef struct {int x0,y0,x1,y1;} render_win_t;
typedef struct {int unused;} render_output_t;
typedef struct {
    int64_t burst_start,expand_us,px,windows;
    render_output_t output;
    int next,overlay_count;
    const void *scene;
    uint16_t *bufs[LCD_PIPE];
} frame_out_t;
static int expands,edges,texts,writes;
static int64_t esp_timer_get_time(void) {return 37;}
static uint16_t ordered(uint16_t value,bool swap) {return swap ? (uint16_t)((value<<8)|(value>>8)) : value;}
static void render_output_columns(render_output_t *out,int x0,int x1) {(void)out;assert(x0==0 && x1==480);}
static void display_begin(int x0,int y0,int x1,int y1) {assert(!x0 && !y0 && x1==480 && y1==16);}
static void render_expand_2x(render_output_t *out,uint16_t *pixels,int x,int y,int w,int h,bool swap) {
    (void)out;(void)x;(void)y;assert(swap==!CONFIG_IDF_TARGET_ESP32S3);expands++;
    for(int i=0;i<w*h;i++)pixels[i]=ordered(0x7139,swap);
}
static void render_edge_overlay(const void *scene,uint16_t *pixels,int x,int y,int w,int h,bool swap) {
    (void)scene;(void)x;(void)y;assert(swap==!CONFIG_IDF_TARGET_ESP32S3);edges++;
    for(int i=0;i<w*h;i++)assert(pixels[i]==ordered(0x7139,swap));
    pixels[0]=ordered(0x52ac,swap);
}
static void render_text_overlay(const void *scene,uint16_t *pixels,int x,int y,int w,int h,bool swap) {
    (void)scene;(void)x;(void)y;(void)w;(void)h;assert(swap==!CONFIG_IDF_TARGET_ESP32S3);texts++;
    assert(pixels[0]==ordered(0x52ac,swap));pixels[0]=ordered(0x95b7,swap);
}
static void display_write(const uint16_t *pixels,int count,bool last) {
    assert(count==480*8 && last==(writes==1));
    assert(pixels[0]==ordered(0x95b7,!CONFIG_IDF_TARGET_ESP32S3));writes++;
}
static void display_end(void) {assert(writes==2);}
'''
order_test = r'''
int main(void) {
    uint16_t buffers[3][480*8];
    frame_out_t frame={.overlay_count=1,.bufs={buffers[0],buffers[1],buffers[2]}};
    const render_win_t window={0,0,480,16};
    send_window(&frame,&window);
    assert(expands==2 && edges==2 && texts==2 && writes==2);
    assert(frame.px==480*16 && frame.windows==1);
    assert(frame.burst_start==(CONFIG_IDF_TARGET_ESP32S3 ? 0 : 37));
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-present-order-') as temporary:
    directory = Path(temporary)
    path, binary = directory / 'order.c', directory / 'order'
    path.write_text(order_fixture + send_window + order_test)
    for target in (0, 1):
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DCONFIG_IDF_TARGET_ESP32S3={target}',
                        str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
print('presentation: C6 wire order unchanged; S3 expansion, edge and full-resolution text use host order')
