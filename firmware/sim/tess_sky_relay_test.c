// Real input drives captures; old channels keep moving without feeding unfinished stars.
#include "tess_sky_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void old_streams_do_not_fill_new_stars(sky_world_t w, const sky_particle_t *particles) {
    sky_particle_t p[SKY_PARTICLES]; memcpy(p, particles, sizeof p);
    w.emitting=false; // Isolate the already established links.
    float fill[SKY_STARS];
    for (int i=0;i<w.count_stars;i++) fill[i]=w.star[i].fill;
    for (int i=0;i<SKY_PARTICLES;i++) if (!p[i].channel) p[i].wait=60000;
    sky_force_t none={0};
    for (int tick=0;tick<600;tick++) sky_stream_step(&w,p,SKY_PARTICLES,SKY_STEP_S,&none,NULL);
    for (int i=0;i<w.count_stars;i++) assert(w.star[i].fill<=fill[i]);
    unsigned owners=0;
    for (int i=0;i<SKY_PARTICLES;i++) if (p[i].channel) {
        owners|=1u<<(p[i].channel-1);
        assert(p[i].age!=particles[i].age);
    }
    assert(owners==(1u<<w.channels)-1);
}
static void visible_continuity(const sky_particle_t *p, const sky_particle_t *before) {
    for(int i=0;i<SKY_PARTICLES;i++)if(p[i].channel && !p[i].wait && !before[i].wait)
        assert(abs(p[i].x-before[i].x)<=100 && abs(p[i].y-before[i].y)<=100);
}
static void channel_owners(const sky_world_t *w, const sky_particle_t *p) {
    unsigned counts[SKY_CHANNELS]={0};
    for(int i=0;i<SKY_PARTICLES;i++)if(p[i].channel)counts[p[i].channel-1]++;
    for(int i=0;i<w->channels;i++)assert(counts[i]==SKY_CHANNEL_DOTS);
}
static void settled_links_are_straight(const sky_world_t *w, const sky_particle_t *p) {
    unsigned visible[SKY_CHANNELS]={0};
    for(int i=0;i<SKY_PARTICLES;i++) {
        if(!p[i].channel || p[i].wait)continue;
        const sky_channel_t *c=&w->channel[p[i].channel-1];
        long dx=c->x[1]-c->x[0],dy=c->y[1]-c->y[0];
        long cross=(p[i].x-c->x[0])*dy-(p[i].y-c->y[0])*dx;
        assert(labs(cross)<=labs(dx)+labs(dy));
        visible[p[i].channel-1]++;
    }
    for(int i=0;i<w->channels;i++)assert(visible[i]>=SKY_CHANNEL_DOTS-1);
}
static void capture_and_continuity(void) {
    sky_recipe_t r={.elements=8}; sky_world_t w; sky_compose(&w,4,&r);
    assert(w.star[0].fill==1 && w.star[0].sang && w.emitter==0 && !w.channels);
    sky_particle_t p[SKY_PARTICLES], before[SKY_PARTICLES]; sky_stream_launch(&w,p,SKY_PARTICLES,NULL);
    sky_force_t force={0}; unsigned channels=0, emitter=0;
    for(int tick=0;tick<2400 && !sky_solved(&w);tick++) {
        if(tick%8==0)sky_solution(&w,&force);
        memcpy(before,p,sizeof p);
        sky_stream_step(&w,p,SKY_PARTICLES,SKY_STEP_S,&force,NULL);
        visible_continuity(p,before);
        if(w.channels==channels)continue;
        assert(w.channels>channels && w.channels<=SKY_CHANNELS);
        const sky_channel_t *c=&w.channel[channels]; assert(c->from==emitter);
        assert(w.source.x==w.star[w.emitter].x && w.source.y==w.star[w.emitter].y);
        channel_owners(&w,p);
        old_streams_do_not_fill_new_stars(w,p);
        channels=w.channels;emitter=w.emitter;
    }
    assert(sky_solved(&w) && channels==SKY_CHANNELS);
    // Multiple complete loops: visible dots never jump from a destination back to its source.
    for(int tick=0;tick<1800;tick++) {
        memcpy(before,p,sizeof p); sky_stream_step(&w,p,SKY_PARTICLES,SKY_STEP_S,&force,NULL);
        visible_continuity(p,before);
        if(tick>120)settled_links_are_straight(&w,p);
    }
}
static void every_emitter_target_pair_works_with_the_smallest_active_pool(void) {
    for(int id=1;id<=SKY_CHARTS;id++)for(unsigned seed=1;seed<=8;seed++) {
        sky_world_t base;sky_recipe_t r={.elements=(uint8_t)id};sky_compose(&base,seed,&r);
        for(int from=0;from<base.count_stars;from++)for(int to=1;to<base.count_stars;to++) {
            if(from==to)continue;
            sky_world_t w=base;sky_emitter(&w,(unsigned)from);
            for(int i=0;i<w.count_stars;i++){w.star[i].fill=i==to?0:1;w.star[i].sang=i!=to;}
            sky_particle_t p[52];sky_stream_launch(&w,p,52,NULL);
            sky_force_t force;sky_solution(&w,&force);w.chart=0; // isolate one physical route, without relay capture
            for(int tick=0;tick<900 && w.star[to].fill<SKY_FULL;tick++)sky_stream_step(&w,p,52,1.f/30,&force,NULL);
            if(w.star[to].fill<SKY_FULL)fprintf(stderr,"route id=%d seed=%u turn=%u from=%d to=%d fill=%f\n",id,seed,w.turn,from,to,w.star[to].fill);
            assert(w.star[to].fill>=SKY_FULL);
        }
    }
}
static void straight_links_never_pause(void) {
    sky_world_t w={.kind=SKY_AURORA,.count_stars=2,.channels=1,.feed_scale=1};
    w.star[0].fill=w.star[1].fill=1; w.star[0].sang=w.star[1].sang=true;
    sky_channel_t *c=&w.channel[0];
    *c=(sky_channel_t){.x={600,3100},.y={2900,800},.step=351,.to=1};
    sky_particle_t p[SKY_CHANNEL_DOTS]={0}; sky_force_t none={0};
    for(int i=0;i<SKY_CHANNEL_DOTS;i++) {
        p[i].channel=1;p[i].age=(uint16_t)(i*65536u/SKY_CHANNEL_DOTS);
        p[i].x=c->x[0]+(c->x[1]-c->x[0])*(int)p[i].age/65536;
        p[i].y=c->y[0]+(c->y[1]-c->y[0])*(int)p[i].age/65536;
    }
    for(int tick=0;tick<2400;tick++) {
        sky_stream_step(&w,p,SKY_CHANNEL_DOTS,tick%2 ? SKY_STEP_S : 1.f/30,&none,NULL);
        unsigned visible=0, sectors=0;
        for(int i=0;i<SKY_CHANNEL_DOTS;i++) {
            if(p[i].wait)continue;
            visible++;
            long cross=(long)(p[i].x-c->x[0])*(c->y[1]-c->y[0])-(long)(p[i].y-c->y[0])*(c->x[1]-c->x[0]);
            assert(labs(cross)<5000); // less than a quarter pixel from the actual star-to-star line
            sectors|=1u<<(p[i].age/16384);
        }
        assert(visible>=SKY_CHANNEL_DOTS-1 && sectors==15);
    }
}
static void source_keeps_emitting_after_handoff(void) {
    for(int n=52;n<=SKY_PARTICLES;n+=60) {
        sky_world_t w={.kind=SKY_AURORA,.count_stars=2,.feed_scale=1,.rng=42};
        w.star[0]=(sky_star_t){.x=100,.y=80,.fill=1,.sang=true};
        w.star[1]=(sky_star_t){.x=340,.y=390,.fill=1,.sang=true};
        sky_emitter(&w,0);
        sky_particle_t p[SKY_PARTICLES]; sky_stream_launch(&w,p,n,NULL);
        sky_force_t none={0}; int gap=0;
        for(int tick=0;tick<1800;tick++) {
            if(tick==900) {sky_emitter(&w,1);gap=0;}
            sky_stream_step(&w,p,n,SKY_STEP_S,&none,NULL);
            bool emitted=false;
            for(int i=0;i<n;i++) if(p[i].age==0 && p[i].wait==1) {
                assert(hypotf(p[i].x/SKY_Q3-w.source.x,p[i].y/SKY_Q3-w.source.y)<9);
                emitted=true;
            }
            gap=emitted?0:gap+1;
            assert(gap<480/n);
            if(tick==900)assert(emitted);
        }
    }
}
int main(void) {
    straight_links_never_pause(); source_keeps_emitting_after_handoff();
    capture_and_continuity(); every_emitter_target_pair_works_with_the_smallest_active_pool();
    puts("Skies relay: prefilled source, every physical route with 52 dots, six persistent channels, no hidden fill or visible teleport");
}
