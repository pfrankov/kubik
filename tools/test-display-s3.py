#!/usr/bin/env python3
"""Verify the actual S3 sender with host threads, delayed DMA and bounded read credits."""
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='kubik-display-s3-') as temporary:
    binary = str(Path(temporary) / 'display-test')
    units = ['main/display_s3', 'main/display_s3_tx', 'sim/display_s3_tx_mocks', 'sim/display_s3_tx_test']
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-fsanitize=address,undefined', '-DCONFIG_IDF_TARGET_ESP32S3=1',
                    '-I' + str(ROOT / 'firmware/sim/display_s3_stubs'), '-I' + str(ROOT / 'firmware/main'),
                    '-I' + str(ROOT / 'firmware/sim'), *[str(ROOT / f'firmware/{unit}.c') for unit in units],
                    '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=20)
    for mode in ('start', 'credit', 'idle'):
        result = subprocess.run([binary, mode], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        assert result.returncode == -signal.SIGABRT, (mode, result.returncode)
print('S3 TX required START/credit/DONE waits fail explicitly at their bounded timeout')

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
