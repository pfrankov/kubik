// Uniform output spans remain pixel-exact across cache/window and byte-order changes.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../main/render.h"
static bool sparse, dense, gradient;
static unsigned reads;
static int flat_color = -1;
static uint16_t pixel(int x, int y) {
    static const uint16_t colors[]={0,0x1234,0xffff,0x7bef};
    if (flat_color >= 0) return (uint16_t)flat_color;
    if (gradient) return colors[y%4];
    if (dense) return (uint16_t)((x * 4051u) ^ (y * 9781u) ^ (x * y * 73u));
    if(sparse)return x%53==0 || (x%61==0 && y%3==0) ? colors[(x+y/3)%4] : 0;
    uint16_t p=colors[(y/3)%4];
    return y%7==0 && x==R_W-1 ? 0xf800 : p;
}
static void read_row(void *ctx,int y,uint16_t *out,int xa,int xb) {
    (void)ctx;assert(y>=0 && y<R_H && xa>=0 && xb<=R_W && xa<xb);
    reads++;memset(out,0xA5,(R_W+1)*sizeof *out);
    for(int x=xa;x<xb;x++)out[x]=pixel(x,y);
}
static uint16_t middle(uint16_t a,uint16_t b) {
    return (uint16_t)((a&b)+(((a^b)&0xf7deu)>>1));
}
static uint16_t expected(int x,int y) {
    int sx=x/2,sy=y/2,nx=sx+1<R_W?sx+1:sx,ny=sy+1<R_H?sy+1:sy;
    uint16_t a=pixel(sx,sy),b=pixel(sx,ny);
    if(x&1){a=middle(a,pixel(nx,sy));b=middle(b,pixel(nx,ny));}
    return y&1?middle(a,b):a;
}
static void window(render_output_t *state,int x,int y,int width,bool swap) {
    uint16_t out[480*4];
    render_output_columns(state,x,x+width);
    render_expand_2x(state, out, x, y, width, 4, width, swap);
    for(int row=0;row<4;row++)for(int col=0;col<width;col++) {
        uint16_t want=expected(x+col,y+row);
        if(swap)want=(uint16_t)((want<<8)|(want>>8));
        assert(out[row*width+col]==want);
    }
}
static void strided_window(int x0, int y0, int width, int rows, bool swap) {
    _Alignas(4) static uint16_t guarded[8 + UI_RENDER_W * UI_OUTPUT_H + 8];
    static render_output_t state;
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++) guarded[i] = 0xA55A;
    render_output_begin(&state, read_row, NULL);
    render_output_columns(&state, x0, x0 + width);
    render_expand_2x(&state, guarded + 8 + x0, x0, y0, width, rows, UI_RENDER_W, swap);
    for (int y = 0; y < UI_OUTPUT_H; y++) for (int x = 0; x < UI_RENDER_W; x++) {
        uint16_t want = 0xA55A;
        if (y < rows && x >= x0 && x < x0 + width) {
            want = expected(x, y + y0);
            if (swap) want = rgb565_bswap(want);
        }
        assert(guarded[8 + y * UI_RENDER_W + x] == want);
    }
    for (int i = 0; i < 8; i++) {
        assert(guarded[i] == 0xA55A);
        assert(guarded[8 + UI_RENDER_W * UI_OUTPUT_H + i] == 0xA55A);
    }
}
static void strides(void) {
    for (int pattern = 0; pattern < 4; pattern++) {
        flat_color = pattern < 2 ? pattern * 0x1234 : -1; sparse = pattern == 3;dense=false;
        for (int swap = 0; swap < 2; swap++) {
            strided_window(0, 0, UI_OUTPUT_W, UI_OUTPUT_H, swap);
            strided_window(2, 2, 18, 18, swap);
            strided_window(UI_OUTPUT_W - 2, UI_OUTPUT_H - 2, 2, 2, swap);
        }
    }
}
static void cached_reads(void) {
    static render_output_t state;
    _Alignas(4) uint16_t out[UI_RENDER_W * 2];
    flat_color=-1;sparse=false;gradient=false;dense=true;reads=0;
    render_output_begin(&state,read_row,NULL);
    render_output_columns(&state,2,20);
    render_expand_2x(&state,out,2,4,18,2,18,false);assert(reads==2);
    render_expand_2x(&state,out,2,4,18,2,18,false);assert(reads==2);
    render_output_columns(&state,4,12);
    render_expand_2x(&state,out,4,4,8,2,8,false);assert(reads==2);
    render_output_columns(&state,2,20);
    render_expand_2x(&state,out,2,4,18,2,18,false);assert(reads==4);
    render_expand_2x(&state,out,2,4,18,2,18,true);assert(reads==4);
    render_expand_2x(&state,out,2,4,18,2,18,true);assert(reads==4);
    render_output_columns(&state,UI_RENDER_W-2,UI_RENDER_W);
    render_expand_2x(&state,out,UI_RENDER_W-2,4,2,2,2,true);assert(reads==6);
    for(int y=0;y<2;y++)for(int x=0;x<2;x++)
        assert(out[y*2+x]==rgb565_bswap(expected(UI_RENDER_W-2+x,4+y)));
    dense=false;
    puts("output: native decode is cached by row/range; swap reuses decoded pairs, expansion and last-column replication stay bounded");
}
static void exact_black_metadata(void) {
    static render_output_t state;
    static uint16_t lit[R_H];
    flat_color=-1; sparse=false; dense=false; gradient=false;
    for(int pattern=0;pattern<4;pattern++) {
        gradient=pattern==1; sparse=pattern==2; dense=pattern==3;
        for(int y=0;y<R_H;y++) {
            lit[y]=0;
            for(int x=0;x<R_W;x++)lit[y]|=pixel(x,y);
        }
        for(int y=0;y<=UI_OUTPUT_H-4;y+=2)for(int swap=0;swap<2;swap++) {
            render_output_begin(&state,read_row,NULL);
            render_output_black_rows(&state,lit); reads=0;
            unsigned want=0;
            for(int sy=y/2;sy<=y/2+2 && sy<R_H;sy++)want+=lit[sy]!=0;
            window(&state,2,y,18,swap); assert(reads==want);
            if(y/2+2<R_H)want+=(lit[y/2]!=0)+(lit[y/2+2]!=0);
            window(&state,2,y,2,!swap); assert(reads==want);
            // Widening invalidates both cached rows, including skipped black ones.
            window(&state,0,y,UI_RENDER_W,swap);
            window(&state,UI_RENDER_W-2,y,2,!swap);
        }
    }
    // Metadata is frame-scoped, never inherited by a generic row reader.
    gradient=false; sparse=false; dense=false; flat_color=0;
    memset(lit,0,sizeof lit);
    render_output_begin(&state,read_row,NULL);
    render_output_black_rows(&state,lit); reads=0;
    window(&state,0,0,UI_RENDER_W,false); assert(reads==0);
    flat_color=0x1234;
    render_output_begin(&state,read_row,NULL); assert(!state.row_lit);
    window(&state,0,0,UI_RENDER_W,false); assert(reads==3);
    for(int y=0;y<R_H;y++)lit[y]=1;
    render_output_black_rows(&state,lit); reads=0;
    window(&state,0,0,UI_RENDER_W,true); assert(reads==3);
    flat_color=0; memset(lit,0,sizeof lit);
    render_output_black_rows(&state,lit); reads=0;
    window(&state,0,0,UI_RENDER_W,false); assert(reads==0);
    flat_color=-1;
    puts("output: exact black-row metadata skips only proven zero rows, preserving neighbours, cache windows, swaps and frame reset");
}
int main(void) {
    static render_output_t state;
    for(int pass=0;pass<3;pass++) {
      sparse=pass==1;dense=pass==2;render_output_begin(&state,read_row,NULL);
      for(int y=0;y<=UI_OUTPUT_H-4;y+=2) {
        // Native odd origins, two-pixel windows, shrinking/expanding cache range,
        // final replicated source pixel, flat/nonflat rows and alternating byte order.
        window(&state,2,y,18,false);window(&state,2,y,2,true);
        window(&state,0,y,UI_RENDER_W,false);window(&state,UI_RENDER_W-12,y,12,true);
      }
    }
    strides();cached_reads();exact_black_metadata();
    gradient=true;flat_color=-1;sparse=false;dense=false;
    render_output_begin(&state,read_row,NULL);
    for(int y=0;y<=UI_OUTPUT_H-4;y+=2) {
        window(&state,0,y,UI_RENDER_W,false);
        window(&state,0,y,UI_RENDER_W,true);
        window(&state,2,y,18,false);
    }
    for(int swap=0;swap<2;swap++)strided_window(2,2,18,18,swap);
    gradient=false;
    puts("output: uniform distinct native rows interpolate once, including black/color pairs, preserving both byte orders");
    puts("output: real full/partial strided expansion preserves row/outer canaries in zero/colored/sparse spans");
    puts("output: zero/colored flat and sparse spans, changing rows, interpolation edges, cache widths and byte order match scalar reference");
}
