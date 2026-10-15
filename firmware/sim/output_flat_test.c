// Uniform output spans remain pixel-exact across cache/window and byte-order changes.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../main/render.h"
static bool sparse;
static uint16_t pixel(int x, int y) {
    static const uint16_t colors[]={0,0x1234,0xffff,0x7bef};
    if(sparse)return x%53==0 || (x%61==0 && y%3==0) ? colors[(x+y/3)%4] : 0;
    uint16_t p=colors[(y/3)%4];
    return y%7==0 && x==R_W-1 ? 0xf800 : p;
}
static void read_row(void *ctx,int y,uint16_t *out,int xa,int xb) {
    (void)ctx;assert(xa>=0 && xb<=R_W && xa<xb);
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
    render_expand_2x(state,out,x,y,width,4,swap);
    for(int row=0;row<4;row++)for(int col=0;col<width;col++) {
        uint16_t want=expected(x+col,y+row);
        if(swap)want=(uint16_t)((want<<8)|(want>>8));
        assert(out[row*width+col]==want);
    }
}
int main(void) {
    static render_output_t state;
    for(int pass=0;pass<2;pass++) {
      sparse=pass;render_output_begin(&state,read_row,NULL);
      for(int y=0;y<=476;y+=2) {
        // Native odd origins, two-pixel windows, shrinking/expanding cache range,
        // final replicated source pixel, flat/nonflat rows and alternating byte order.
        window(&state,2,y,18,false);window(&state,2,y,2,true);
        window(&state,0,y,480,false);window(&state,468,y,12,true);
      }
    }
    puts("output: zero/colored flat and sparse spans, changing rows, interpolation edges, cache widths and byte order match scalar reference");
}
