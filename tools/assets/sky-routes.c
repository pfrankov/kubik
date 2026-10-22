// Offline only: find ordinary tilt + one-finger routes using the production physics.
#include "tess_sky_internal.h"
#include <stdio.h>
#include <stdlib.h>

static int attempt(const sky_world_t *base, int from, int to, int gx, int gy, int ox, int oy, int limit) {
    sky_world_t w=*base; sky_emitter(&w,(unsigned)from); w.chart=0;
    for(int i=0;i<w.count_stars;i++) { w.star[i].fill=i==to?0:1; w.star[i].sang=i!=to; }
    sky_particle_t p[52]; sky_stream_launch(&w,p,52,NULL);
    sky_force_t f={.gx=gx*.075f,.gy=gy*.075f,.count=1,
        .touch={{.x=w.star[to].x+ox,.y=w.star[to].y+oy,.shown=1,.spin=1,.down=true}}};
    for(int tick=0;tick<limit;tick++) {
        sky_stream_step(&w,p,52,1.f/30,&f,NULL);
        if(w.star[to].fill>=SKY_FULL)return tick;
    }
    return limit;
}
static int robust_attempt(const sky_world_t *base, int from, int to, int gx, int gy, int ox, int oy, int limit) {
    int worst=0;
    for(unsigned seed=1;seed<=8;seed++) {
        sky_world_t w; sky_recipe_t r={.elements=base->chart}; sky_compose(&w,seed,&r);
        // Rotate the ordinary input with the whole chart, exactly as sky_solution does.
        int x=gx,y=gy,dx=ox,dy=oy;
        for(int turn=0;turn<w.turn;turn++) { int old=x;x=-y;y=old; old=dx;dx=-dy;dy=old; }
        int ticks=attempt(&w,from,to,x,y,dx,dy,limit);
        if(ticks>=limit)return limit;
        if(ticks>worst)worst=ticks;
    }
    return worst;
}
static void route(const sky_world_t *w, int from, int to) {
    int bx=0,by=0,ox=0,oy=0,best=900;
    const int offsets[9][2]={{0,0},{20,0},{-20,0},{0,20},{0,-20},{40,0},{-40,0},{0,40},{0,-40}};
    for(int offset=0;offset<9;offset++) {
    for(int gx=-4;gx<=4;gx++)for(int gy=-4;gy<=4;gy++) {
        if(gx*gx+gy*gy>16 || from==to)continue;
        int ticks=robust_attempt(w,from,to,gx,gy,offsets[offset][0],offsets[offset][1],best);
        if(ticks<best) { best=ticks;bx=gx;by=gy;ox=offsets[offset][0];oy=offsets[offset][1]; }
    }
    }
    if(from!=to && best==900) { fprintf(stderr,"Unreachable: chart %u, %d -> %d\n",w->chart,from,to);exit(1); }
    printf("[%d,%d,%d,%d]",bx,by,ox,oy);
}
static void chart(int id) {
    sky_world_t w; sky_recipe_t r={.elements=(uint8_t)id}; sky_compose(&w,4,&r);
    printf("[");
    for(int from=0;from<w.count_stars;from++) {
        if(from)printf(",");printf("[");
        for(int to=0;to<w.count_stars;to++) { if(to)printf(",");route(&w,from,to); }
        printf("]");
    }
    printf("]");
}
int main(void) {
    printf("[");
    for(int id=1;id<=SKY_CHARTS;id++) { if(id>1)printf(",");chart(id); }
    puts("]");
}
