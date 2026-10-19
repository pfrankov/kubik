// Real input drives captures; old channels keep moving without feeding unfinished stars.
#include "tess_sky_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void old_streams_do_not_fill_new_stars(sky_world_t w, const sky_particle_t *particles) {
    sky_particle_t p[SKY_PARTICLES]; memcpy(p, particles, sizeof p);
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
    for(int i=0;i<w->channels;i++)assert(counts[i]>0 && counts[i]<=SKY_CHANNEL_DOTS);
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
static int curve_reference(const int16_t control[4], unsigned phase) {
    int a[4];for(int i=0;i<4;i++)a[i]=control[i];
    for(int n=3;n>0;n--)for(int i=0;i<n;i++)a[i]+=(a[i+1]-a[i])*(int)phase/65536;
    return a[0];
}
static void unrolled_curve_keeps_every_phase_exact(void) {
    sky_force_t none={0};
    for(int shape=0;shape<32;shape++) {
        sky_world_t w={.kind=SKY_AURORA,.count_stars=1,.channels=1,.rng=shape+1,.feed_scale=1};
        w.star[0].fill=1;w.star[0].sang=true;w.channel[0].step=1;
        for(int i=0;i<4;i++) { w.channel[0].x[i]=(int16_t)((int)(sky_rand(&w)%11000)-4000);w.channel[0].y[i]=(int16_t)((int)(sky_rand(&w)%11000)-4000); }
        for(unsigned phase=0;phase<65536;phase++) {
            sky_particle_t p={.channel=1,.age=(uint16_t)(phase-1)};
            sky_stream_step(&w,&p,1,SKY_STEP_S,&none,NULL);
            assert(p.x==curve_reference(w.channel[0].x,phase) && p.y==curve_reference(w.channel[0].y,phase));
        }
    }
}
int main(void) {
    unrolled_curve_keeps_every_phase_exact();
    capture_and_continuity(); every_emitter_target_pair_works_with_the_smallest_active_pool();
    puts("Skies relay: prefilled source, every physical route with 52 dots, six persistent channels, no hidden fill or visible teleport");
}
