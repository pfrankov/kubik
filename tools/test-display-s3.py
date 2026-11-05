#!/usr/bin/env python3
"""Check direct S3 row composition, padding clips, complete-frame TE and delayed DMA ownership."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'firmware/main/display_s3.c').read_text()
for header in ('board.h', 'esp_heap_caps.h', 'esp_timer.h', 'frame_pacing_s3.h', 'esp_log.h'):
    source = source.replace(f'#include "{header}"\n', '')
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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
    assert((bytes==384*448*2 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)) ||
           (bytes==368*8*2 && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT)));
    if(allocation_call==allocation_fail) return NULL;
    void *p=malloc(bytes); assert(p); held++; return p;
}
static void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps) {
    void *p=heap_caps_malloc(count*bytes,caps); if(p)memset(p,0,count*bytes); return p;
}
static void heap_caps_free(void *p) { if(p)held--; free(p); }
static uint16_t screen[448][368], expected[448][384];
static bool raw_open, first_of_burst;
static int te_syncs, idle_waits;
#define ESP_ERROR_CHECK(value) assert((value)==0)
static void verify_native_ready(void);
static bool dma_source(const uint16_t *data);
static unsigned dma_seen;
static int64_t mock_time=123456,te_delay,dma_delay,row_work;
static int64_t esp_timer_get_time(void) {return mock_time;}
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
    assert(te_syncs>0);
    if(first_of_burst) {verify_native_ready();first_of_burst=false;}
    unchanged(); assert(!raw_open && x0>=0 && x1<=368 && x0<x1 && y0>=0 && y1<=448 && y0<y1);
    assert(x0%16==0 && x1%16==0 && y0%8==0 && y1%8==0);
    raw_open=true;raw_x0=x0;raw_x1=x1;raw_y=y0;raw_y1=y1;windows++;
}
void board_display_write(const uint16_t *data,int count,bool last) {
    unchanged(); assert(raw_open && count==(raw_x1-raw_x0)*8 && raw_y+8<=raw_y1 && dma_source(data));
    for(int i=0;i<pending;i++)assert(data!=queue[i].data);
    if(pending==2)complete_one();
    queue[pending++]=(struct queued){data,count,raw_x0,raw_y,raw_x1-raw_x0,hash(data,count)};
    raw_y+=8; assert(last==(raw_y==raw_y1));writes++;
}
void board_display_end(void) {unchanged();assert(raw_open && raw_y==raw_y1);raw_open=false;}
void board_display_wait_idle(void) {
    bool active=pending!=0;
    while(pending)complete_one();
    if(active)mock_time+=dma_delay;
    idle_waits++;
}
static int frame_pacing_s3_sync(void) {
    verify_native_ready(); assert(!pending && !raw_open);
    te_syncs++;mock_time+=te_delay;return 0;
}
'''
checks = r'''
static bool dma_source(const uint16_t *data) {
    for(int i=0;i<3;i++)if(data==s_dma[i]) {dma_seen|=1u<<i;return true;}
    return false;
}
static uint16_t pattern(int x,int y) {
    return (uint16_t)(((unsigned)x*263u+(unsigned)y*977u)^((unsigned)x<<9)^((unsigned)y<<3)^0x95b7u);
}
static void verify_native_ready(void) {
    // Every composed window, including later damage and retained gaps in its hull,
    // must contain its final wire-order pixels before the fresh TE or first SPI command.
    for(int y=0;y<448;y++)for(int x=0;x<368;x++)assert(s_native[y*384+x]==expected[y][x]);
}
static void verify(void) {
    assert(!pending && !raw_open); // present owns the complete DMA burst, including its tail.
    verify_native_ready();
    for(int y=0;y<448;y++)for(int x=0;x<368;x++)assert(screen[y][x]==expected[y][x]);
}
static void write_rect(int x0,int y0,int x1,int y1) {
    display_s3_begin(x0,y0,x1,y1);
    for(int y=y0;y<y1;y++) {
        uint16_t *row=display_s3_next_row();
        assert(row==s_native+y*384+x0);
        memcpy(row,&expected[y][x0],(size_t)(x1-x0)*2);
        mock_time+=row_work;
        assert(s_y==y+1);
        for(int x=x0;x<x1;x++)assert(s_native[y*384+x]==expected[y][x]);
    }
    display_s3_end();
}
static void present(bool visible) {
    int before=windows,prior_sync=te_syncs,prior_idle=idle_waits;
    first_of_burst=true;
    int64_t started=display_s3_present();
    assert((started!=0)==visible && (te_syncs==prior_sync+1)==visible);
    if(visible)assert(!first_of_burst && windows>before && idle_waits==prior_idle+1);
    else assert(first_of_burst && windows==before && idle_waits==prior_idle);
    verify();
}
static void draw(int x0,int y0,int x1,int y1) {
    display_s3_begin_frame();write_rect(x0,y0,x1,y1);present(x0<368);
}
static void fill(int x0,int y0,int x1,int y1,uint16_t value) {
    for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)expected[y][x]=value;
}
static void touch_mapping(void) {
    // Native pixel centres select design coordinates over the complete rectangle.
    for(int y=-1;y<=448;y++)for(int x=-1;x<=368;x++) {
        int16_t lx=-7,ly=-9;
        bool inside=x>=0 && x<368 && y>=0 && y<448;
        assert(display_s3_touch(x,y,&lx,&ly)==inside);
        if(!inside) {assert(lx==-7 && ly==-9);continue;}
        int expected_x=(int)((x+.5)*480./368.);
        int expected_y=(int)((y+.5)*584./448.);
        assert(lx==expected_x && ly==expected_y && lx>=0 && lx<480 && ly>=0 && ly<584);
    }
    static const int corners[][4]={{0,0,0,0},{367,0,479,0},{0,447,0,583},{367,447,479,583},{184,224,240,292}};
    for(unsigned i=0;i<sizeof corners/sizeof corners[0];i++) {
        int16_t x=-1,y=-1;
        assert(display_s3_touch(corners[i][0],corners[i][1],&x,&y) && x==corners[i][2] && y==corners[i][3]);
    }
    int16_t x=-7,y=-9;
    assert(!display_s3_touch(0,0,NULL,&y) && y==-9);
    assert(!display_s3_touch(0,0,&x,NULL) && x==-7);
}
static void full_pattern(void) {
    for(int y=0;y<448;y++)for(int x=0;x<384;x++)expected[y][x]=pattern(x,y);
    draw(0,0,384,448);
    assert(windows==1 && writes==56 && te_syncs==1 && dma_seen==7);
    display_s3_profile_t p=display_s3_last_profile();
    assert(p.input_pixels==384*448 && p.native_pixels==368*448 && p.windows==1);
    assert(p.render_us==0 && p.te_us==0 && p.burst_us==0);
}
static void tiny_and_edge(void) {
    int before=windows;
    display_s3_begin_frame();expected[1][1]=0xf800;write_rect(1,1,2,2);
    assert(windows==before);expected[447][367]=0x07e0;write_rect(367,447,368,448);
    assert(windows==before);present(true);assert(windows==before+2);
    display_s3_profile_t p=display_s3_last_profile();
    assert(p.input_pixels==2 && p.native_pixels==2 && p.windows==2);
    assert(s_left[0]==368 && s_right[0]==0); // successful present clears accumulated damage
    present(false);
}
static void direct_rows(void) {
    display_s3_begin_frame();int before=windows;
    display_s3_begin(120,200,124,204);
    uint16_t *rows[4];
    uint16_t outside=s_native[199*384+120];
    for(int y=0;y<4;y++) {
        rows[y]=display_s3_next_row();
        assert(rows[y]==s_native+(200+y)*384+120);
        for(int x=0;x<4;x++)rows[y][x]=expected[200+y][120+x]=(uint16_t)(0x1529+y*0x217);
        for(int old=0;old<=y;old++)for(int x=0;x<4;x++)assert(rows[old][x]==expected[200+old][120+x]);
        assert(s_native[199*384+120]==outside && windows==before);
    }
    verify_native_ready();
    uint32_t composed=s_profile.native_pixels;display_s3_end();assert(s_profile.native_pixels==composed);present(true);
}
static void row_without_window(void) {display_s3_next_row();}
static void incomplete_window(void) {display_s3_begin(120,200,124,204);display_s3_next_row();display_s3_end();}
static void excess_row(void) {
    display_s3_begin(120,200,124,202);display_s3_next_row();display_s3_next_row();display_s3_next_row();
}
static void present_open_window(void) {display_s3_begin(120,200,124,204);display_s3_present();}
static void beyond_canvas(void) {display_s3_begin(368,0,385,1);}
static void rejects(void (*operation)(void)) {
    pid_t child=fork();assert(child>=0);
    if(!child) {assert(freopen("/dev/null","w",stderr));operation();_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
}
static void row_contract(void) {
    rejects(row_without_window);rejects(incomplete_window);rejects(excess_row);
    rejects(present_open_window);rejects(beyond_canvas);
}

static void overlap_and_profile(void) {
    display_s3_begin_frame();int before=windows;row_work=3;
    fill(100,100,110,116,0x1bf8);write_rect(100,100,110,116);assert(windows==before && s_profile.render_us==48);
    fill(105,108,113,120,0x7be7);write_rect(105,108,113,120);assert(windows==before && s_profile.render_us==84);
    // Separated columns widen the send hull. Its unchanged middle retains exact native pixels.
    fill(130,111,131,115,0x53a9);write_rect(130,111,131,115);assert(windows==before && s_profile.render_us==96);
    row_work=0;te_delay=73;dma_delay=211;present(true);
    display_s3_profile_t p=display_s3_last_profile();
    assert(p.input_pixels==160+96+4 && p.native_pixels==160+96+4 && p.windows==2);
    assert(p.render_us==96 && p.te_us==73 && p.burst_us==211);
    te_delay=dma_delay=0;
    // Repeat writes count as work, including intersections; a crossing window counts only visible columns.
    display_s3_begin_frame();fill(350,430,384,448,0x7531);write_rect(350,430,384,448);
    p=s_profile;assert(p.input_pixels==34*18 && p.native_pixels==18*18);present(true);
    assert(display_s3_last_profile().windows==1);
}
static void random_and_edges(void) {
    uint32_t random=9183;
    for(int n=0;n<96;n++) {
        random=random*1664525u+1013904223u;int x=(int)(random%384);
        random=random*1664525u+1013904223u;int y=(int)(random%448);
        expected[y][x]=(uint16_t)random;draw(x,y,x+1,y+1);
    }
    fill(0,0,1,448,0xf800);draw(0,0,1,448);
    fill(367,0,368,448,0x001f);draw(367,0,368,448);
    fill(0,0,384,1,0x07e0);draw(0,0,384,1);
    fill(0,447,384,448,0xffff);draw(0,447,384,448);
    static uint16_t partial[448][368];memcpy(partial,screen,sizeof screen);
    draw(0,0,384,448);assert(!memcmp(partial,screen,sizeof screen));
}
static void padding(void) {
    static uint16_t native[448][384],physical[448][368];
    memcpy(native,s_native,sizeof native);memcpy(physical,screen,sizeof physical);
    display_s3_begin_frame();row_work=2;fill(368,0,384,448,0xfa75);write_rect(368,0,384,448);row_work=0;
    present(false);
    for(int y=0;y<448;y++) {
        assert(!memcmp(native[y],s_native+y*384,368*2));
        for(int x=368;x<384;x++)assert(s_native[y*384+x]==0xfa75);
    }
    assert(!memcmp(physical,screen,sizeof physical));
    display_s3_profile_t p=display_s3_last_profile();
    assert(p.input_pixels==16*448 && p.native_pixels==0 && p.windows==0 && p.render_us==2*448 && p.te_us==0 && p.burst_us==0);
}
static void reset(void) {
    int calls=allocation_call;
    memset(screen,0,sizeof screen); // init is called only after the physical panel has been cleared
    assert(display_s3_init() && held==4 && allocation_call==calls);
    memset(expected,0,sizeof expected);verify_native_ready();
    expected[447][367]=0x95b7;draw(367,447,368,448);
    assert(screen[447][367]==0x95b7 && s_native[0]==0);
}
int main(void) {
    assert(UI_W==480 && UI_H==584 && UI_RENDER_W==384 && UI_RENDER_H==448);
    touch_mapping();
    for(int fail=1;fail<=4;fail++) {
        allocation_call=0;allocation_fail=fail;
        assert(!display_s3_init() && held==0);
    }
    allocation_call=0;allocation_fail=0;
    assert(display_s3_init() && held==4 && allocation_call==4);
    full_pattern();tiny_and_edge();direct_rows();row_contract();overlap_and_profile();random_and_edges();padding();reset();
    release_buffers();assert(held==0);
#ifdef ESP_PLATFORM
    s_last_profile=(display_s3_profile_t){.render_us=26000};
    for(int i=0;i<8;i++)report_slow_phases();assert(logged==4);
    mock_time+=10000000;report_slow_phases();assert(logged==5);
    s_last_profile.render_us=25000;report_slow_phases();assert(logged==5);
#endif
    puts("S3 native display: direct native rows, exact wire pixels, right padding, retained hull gaps, complete composition before TE, delayed DMA, touch, profiles and four-buffer failure/reset passed");
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-display-s3-') as temporary:
    directory = Path(temporary)
    path, binary = directory / 'test.c', directory / 'test'
    path.write_text(fixture + source + checks)
    (directory / 'sdkconfig.h').write_text('#define CONFIG_IDF_TARGET_ESP32S3 1\n')
    for flags in ([], ['-DESP_PLATFORM=1']):
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *flags,
                        '-fsanitize=address,undefined', '-DCONFIG_IDF_TARGET_ESP32S3=1', '-I', str(directory),
                        '-I', str(ROOT / 'firmware/main'), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)

# Compile the actual boundary: expansion, edge and text write wire-order RGB565
# on both targets. S3 composes bounded internal chunks, then copies exact rows into the padded native store.
presentation = (ROOT / 'firmware/main/present.c').read_text()
helper_start = presentation.index('static render_output_t *output_for(')
helper_end = presentation.index('\n}\n', helper_start) + 3
send_window = presentation[helper_start:helper_end] + presentation[presentation.index('static void compose_window_rows('):presentation.index('static int larger(')]
order_fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#if CONFIG_IDF_TARGET_ESP32S3
#define LCD_W 384
#else
#define LCD_W 480
#endif
#define UI_RENDER_W LCD_W
#define OUTPUT_ROWS 8
#define LCD_PIPE 3
#ifndef X0
#define X0 16
#endif
#ifndef X1
#define X1 (LCD_W-16)
#endif
#define Y0 8
#ifndef Y1
#define Y1 40
#endif
#define WIDTH (X1-X0)
typedef struct {int x0,y0,x1,y1;} render_win_t;
typedef struct {int marker;} render_output_t;
typedef struct {
    int64_t burst_start,expand_us,px,windows;
    #if !CONFIG_IDF_TARGET_ESP32S3
    render_output_t output;
    #endif
    int next,overlay_count;
    const void *scene;
    uint16_t *bufs[LCD_PIPE];
} frame_out_t;
static int expands,edges,texts,writes,begins,ends;
static render_output_t *seen_output;
static int64_t esp_timer_get_time(void) {return 37;}
static uint16_t wire(uint16_t value) {return (uint16_t)((value<<8)|(value>>8));}
static uint16_t expanded_pixel(int x,int y) {return wire((uint16_t)(0x7139u+(unsigned)x*37u+(unsigned)y*53u));}
static uint16_t edge_pixel(int y) {return wire((uint16_t)(0x52acu^((unsigned)y*19u)));}
static uint16_t text_pixel(int y) {return wire((uint16_t)(0x95b7u^((unsigned)y*37u)));}
static void render_output_columns(render_output_t *out,int x0,int x1) {
    assert(out && x0==X0 && x1==X1);seen_output=out;out->marker=0x7139;
}
#if CONFIG_IDF_TARGET_ESP32S3
static uint16_t native[64][384];
static uint16_t *seen_stage;
static int row_at;
static void display_begin(int x0,int y0,int x1,int y1) {
    assert(x0==X0 && y0==Y0 && x1==X1 && y1==Y1);begins++;row_at=y0;
}
static uint16_t *display_s3_next_row(void) {assert(row_at<Y1);return &native[row_at++][X0];}
static void display_end(void) {assert(row_at==Y1);ends++;}
#else
static void display_begin(int x0,int y0,int x1,int y1) {
    assert(x0==X0 && y0==Y0 && x1==X1 && y1==Y1);begins++;
}
static void display_write(const uint16_t *pixels,int count,bool last) {
    int remaining=Y1-Y0-writes*8,rows=remaining<8 ? remaining : 8;
    assert(count==WIDTH*rows && last==(remaining<=8));
    for(int row=0;row<rows;row++)for(int col=0;col<WIDTH;col++) {
        int y=Y0+writes*8+row;
        assert(pixels[row*WIDTH+col]==(col ? expanded_pixel(X0+col,y) : text_pixel(y)));
    }
    writes++;
}
static void display_end(void) {assert(writes==(Y1-Y0+7)/8);ends++;}
#endif
static void destination(uint16_t *pixels,int x,int y,int w,int h,int stride) {
    assert(x==X0 && w==WIDTH && w%2==0 && h%2==0);
#if CONFIG_IDF_TARGET_ESP32S3
    int remaining=Y1-y;
    assert(y>=Y0 && y<Y1 && (y-Y0)%24==0 && h==(remaining<24 ? remaining : 24) && stride==WIDTH && row_at==y);
    uintptr_t address=(uintptr_t)pixels;
    assert(address<(uintptr_t)native || address>=(uintptr_t)native+sizeof native);
    if(!seen_stage)seen_stage=pixels;
    assert(pixels==seen_stage);
#else
    int remaining=Y1-y;
    (void)pixels;assert(y>=Y0 && y<Y1 && (y-Y0)%8==0 && h==(remaining<8 ? remaining : 8) && stride==WIDTH);
#endif
}
static void render_expand_2x(render_output_t *out,uint16_t *pixels,int x,int y,int w,int h,int stride,bool swap) {
    assert(out==seen_output && out->marker==0x7139 && swap);destination(pixels,x,y,w,h,stride);expands++;
    for(int row=0;row<h;row++)for(int col=0;col<w;col++)pixels[row*stride+col]=expanded_pixel(x+col,y+row);
}
static void render_edge_overlay(const void *scene,uint16_t *pixels,int x,int y,int w,int h,int stride,bool swap) {
    (void)scene;assert(swap);destination(pixels,x,y,w,h,stride);edges++;
    for(int row=0;row<h;row++)for(int col=0;col<w;col++)assert(pixels[row*stride+col]==expanded_pixel(x+col,y+row));
    for(int row=0;row<h;row++)pixels[row*stride]=edge_pixel(y+row);
}
static void render_text_overlay(const void *scene,uint16_t *pixels,int x,int y,int w,int h,int stride,bool swap) {
    (void)scene;assert(swap);destination(pixels,x,y,w,h,stride);texts++;
    for(int row=0;row<h;row++) {
        assert(pixels[row*stride]==edge_pixel(y+row));pixels[row*stride]=text_pixel(y+row);
    }
}
'''
order_test = r'''
int main(void) {
#if CONFIG_IDF_TARGET_ESP32S3
    // S3 must not dereference or rotate the old chunk-buffer pool.
    frame_out_t frame={.overlay_count=1,.next=2,.bufs={NULL,NULL,NULL}};
    for(int y=0;y<64;y++)for(int x=0;x<384;x++)native[y][x]=0x6dab;
#else
    uint16_t buffers[3][LCD_W*8];
    frame_out_t frame={.overlay_count=1,.bufs={buffers[0],buffers[1],buffers[2]}};
#endif
    const render_win_t window={X0,Y0,X1,Y1};
    send_window(&frame,&window);
    assert(begins==1 && ends==1 && frame.px==WIDTH*(Y1-Y0) && frame.windows==1);
    assert(frame.burst_start==(CONFIG_IDF_TARGET_ESP32S3 ? 0 : 37));
    frame_out_t another={0};
#if CONFIG_IDF_TARGET_ESP32S3
    assert(output_for(&another)==seen_output && seen_output->marker==0x7139);
#else
    assert(seen_output==&frame.output && output_for(&another)==&another.output && &another.output!=seen_output);
    assert(another.output.marker==0);
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    assert(expands==(Y1-Y0+23)/24 && edges==expands && texts==expands && writes==0 && row_at==Y1 && frame.next==2 && seen_stage);
    for(int y=0;y<64;y++)for(int x=0;x<384;x++) {
        uint16_t expected=y>=Y0 && y<Y1 && x>=X0 && x<X1 ? (x==X0 ? text_pixel(y) : expanded_pixel(x,y)) : 0x6dab;
        assert(native[y][x]==expected);
    }
#else
    assert(expands==(Y1-Y0+7)/8 && edges==expands && texts==expands && writes==expands);
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='kubik-present-order-') as temporary:
    directory = Path(temporary)
    path, binary = directory / 'order.c', directory / 'order'
    path.write_text(order_fixture + send_window + order_test)
    for target, tail, full in ((0, 40, False), (1, 40, False), (0, 58, False), (1, 58, False),
                               (0, 40, True), (1, 40, True), (0, 58, True), (1, 58, True)):
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DCONFIG_IDF_TARGET_ESP32S3={target}', f'-DY1={tail}',
                        *(['-DX0=0', '-DX1=LCD_W'] if full else []), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)
print('presentation: C6 packed DMA chunks preserved; S3 24-row internal composition and partial tails copy exact native rows with full-width capacity and untouched out-of-window padding')
