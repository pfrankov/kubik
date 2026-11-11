#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
/* ACTUAL_PIPE_ROWS */
#define LCD_W 480
#define LCD_H 480
typedef struct {int x0,y0,x1,y1;} render_win_t;
typedef struct {
    uint16_t *bufs[LCD_PIPE];int next;int64_t burst_start,expand_us,px,windows;
} frame_out_t;
static uint16_t buffers[LCD_PIPE][LCD_W*OUTPUT_ROWS];
static struct {const uint16_t *data;int count;uint32_t hash;} pending[LCD_DMA_CREDITS];
static unsigned pending_n;
static bool reserved;
static unsigned submitted,completed,chunks,compositions,max_inflight;
static int win_pixels,win_sent,win_width;static bool last_seen;
static int64_t clock_now;
static int64_t esp_timer_get_time(void){return ++clock_now;}
static void *output_for(frame_out_t *frame){return frame;}
static void render_output_columns(void *output,int x0,int x1){assert(output && x0>=0 && x1<=LCD_W && x0<x1);}
static uint32_t digest(const uint16_t *data,int count) {
    uint32_t hash=2166136261u;for(int i=0;i<count;i++)hash=(hash^data[i])*16777619u;return hash;
}
static void complete_dma(void) {
    assert(pending_n && digest(pending[0].data,pending[0].count)==pending[0].hash);
    completed+=pending[0].count;pending_n--;
    memmove(pending,pending+1,pending_n*sizeof *pending);
}
static void compose_window_rows(frame_out_t *frame,uint16_t *out,int x,int y,int width,int rows,int stride) {
    (void)frame;assert(stride==width && width*rows<=LCD_W*OUTPUT_ROWS);
    for(unsigned i=0;i<pending_n;i++)assert(out!=pending[i].data);
#ifndef DMA_OLD_WAIT_AFTER_COMPOSE
    assert(reserved); // Acquiring after this point is already too late to protect the pointer.
#endif
    compositions++;
    for(int yy=0;yy<rows;yy++)for(int xx=0;xx<width;xx++)out[yy*stride+xx]=(uint16_t)((y+yy)*487+x+xx+compositions);
}
static void display_begin(int x0,int y0,int x1,int y1) {
    assert(x0>=0 && y0>=0 && x1<=LCD_W && y1<=LCD_H);
    win_width=x1-x0;win_pixels=win_width*(y1-y0);win_sent=0;last_seen=false;
}
static void display_reserve_buffer(void) {
    assert(!reserved);
    if(pending_n==LCD_DMA_CREDITS)complete_dma();
    reserved=true;
}
static void display_write(const uint16_t *data,int count,bool last) {
#ifdef DMA_OLD_WAIT_AFTER_COMPOSE
    display_reserve_buffer();
#endif
    assert(reserved && pending_n<LCD_DMA_CREDITS);reserved=false;
    assert(count>0 && count<=LCD_W*OUTPUT_ROWS && count%win_width==0 && !last_seen);
    pending[pending_n].data=data;pending[pending_n].count=count;pending[pending_n].hash=digest(data,count);
    pending_n++;if(pending_n>max_inflight)max_inflight=pending_n;
    submitted+=count;win_sent+=count;chunks++;last_seen=last;
    assert(last==(win_sent==win_pixels));
}
static void display_end(void){assert(last_seen && win_sent==win_pixels && !reserved);}
/* ACTUAL_SEND_WINDOW */
int main(void) {
    _Static_assert(LCD_PIPE==2 && OUTPUT_ROWS==4 && LCD_DMA_CREDITS==2,"C6 two four-row buffers reserve before composition");
    frame_out_t frame={0};for(int i=0;i<LCD_PIPE;i++)frame.bufs[i]=buffers[i];
    const render_win_t windows[]={{0,0,480,480},{17,13,400,112},{0,0,47,479},{452,17,480,204},{0,0,480,480},{0,0,480,480}};
    unsigned expected=0;
    for(unsigned i=0;i<sizeof windows/sizeof *windows;i++) {
        const render_win_t *w=&windows[i];expected+=(w->x1-w->x0)*(w->y1-w->y0);send_window(&frame,w);
        assert(pending_n); // end() must preserve the tail's ownership across windows.
    }
    while(pending_n)complete_dma();assert(submitted==expected && completed==expected && frame.px==expected);
    assert(max_inflight==LCD_DMA_CREDITS && chunks==compositions && frame.windows==6);
    assert(sizeof buffers==7680);
    puts("C6 DMA: actual window chunker preserves delayed source ownership across full/clipped/odd windows; 2x4 uses 7680 bytes");
}
