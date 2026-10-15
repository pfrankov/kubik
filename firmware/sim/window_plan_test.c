// Window planning may overdraw, but must preserve every dirty interpolation pixel.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/render.h"
static bool covers(const render_win_t *w, int n, int x, int y) {
    for (int i=0;i<n;i++) if(x>=w[i].x0&&x<w[i].x1&&y>=w[i].y0&&y<w[i].y1)return true;
    return false;
}
static void verify_mask(const uint16_t *mask, const render_win_t *windows, int n, int top) {
    for(int y=top;y<top+R_BAND;y++) {
        uint16_t dirty=mask[y]|(y+1<R_H?mask[y+1]:0);
        for(int k=0;k<15;k++) if(dirty>>k&1)
            for(int x=k?32*k-2:0;x<32*(k+1);x++) {
                assert(covers(windows,n,x,2*y)); assert(covers(windows,n,x,2*y+1));
            }
    }
}
static void random_masks(uint16_t *mask, render_win_t *windows) {
    uint32_t seed=31;
    for(int frame=0;frame<2000;frame++) {
        int top=(frame%(R_BANDS))*R_BAND;
        memset(mask,0,R_H*sizeof *mask);
        for(int y=top;y<=top+R_BAND&&y<R_H;y++) {
            seed=seed*1664525u+1013904223u; mask[y]=(uint16_t)(seed&0x7fff);
        }
        int n=render_band_windows(mask,top,top+R_BAND,windows);
        assert(n>=0&&n<=RENDER_BAND_WINDOWS);
        for(int i=0;i<n;i++) {
            assert(windows[i].x0>=0&&windows[i].x1<=480&&windows[i].x0<windows[i].x1);
            assert(windows[i].y0>=2*top&&windows[i].y1<=2*(top+R_BAND));
        }
        verify_mask(mask,windows,n,top);
    }
}
int main(void) {
    uint16_t mask[R_H]={0}; render_win_t windows[RENDER_BAND_WINDOWS];
    assert(render_band_windows(mask,0,R_BAND,windows)==0);
    for(int y=0;y<R_BAND;y++)mask[y]=(y<R_BAND/2)?0x00ff:0x7f80;
    assert(render_band_windows(mask,0,R_BAND,windows)==2); // the useful split saves more than setup
    for(int y=0;y<=R_BAND;y++)mask[y]=0x7fff;
    assert(render_band_windows(mask,0,R_BAND,windows)==1); // dense work stays one window
    random_masks(mask,windows);
    puts("window planner: sparse/dense decisions and 2000 dirty masks preserve all interpolation pixels");
}
