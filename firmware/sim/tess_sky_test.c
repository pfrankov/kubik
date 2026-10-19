#include "tess_sky_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int solved_by(sky_world_t w, bool guide) {
    sky_particle_t p[SKY_PARTICLES];
    sky_force_t force={0};
    if(guide) sky_solution(&w,&force);
    sky_stream_launch(&w,p,SKY_PARTICLES,NULL);
    float held=0;
    for(int i=0;i<(guide?900:450);i++) {
        sky_stream_step(&w,p,SKY_PARTICLES,1.f/30,&force,NULL);
        held=sky_solved(&w)?held+1.f/30:0;
        if(held>=SKY_HOLD_S) return i;
    }
    return -1;
}
static void validate(const sky_world_t *w) {
    assert(w->count_stars<=SKY_STARS && w->count_holes<=SKY_HOLES);
    assert(w->kind==SKY_AURORA ? !w->count_stars : w->count_stars>=1);
    for(int i=0;i<w->count_stars;i++) {
        const sky_star_t *s=&w->star[i];
        assert(s->fill==0 && !s->sang && isfinite(s->hz));
        assert(s->x>=SKY_MARGIN && s->x<=SKY_W-SKY_MARGIN);
        assert(s->y>=SKY_MARGIN && s->y<=SKY_W-SKY_MARGIN);
        for(int j=0;j<i;j++) assert(hypotf(s->x-w->star[j].x,s->y-w->star[j].y)>=79.f);
    }
}
static void free_skies_have_no_targets(void) {
    for(int seed=1;seed<=32;seed++) {
        sky_recipe_t r={.elements=10,.skill=200,.aurora=true,.root=(uint8_t)(seed%5)};
        sky_world_t w;sky_compose(&w,(uint32_t)seed,&r);validate(&w);
        assert(w.kind==SKY_AURORA && w.count_holes==2 && !sky_solved(&w));
        sky_particle_t p[SKY_PARTICLES];sky_stream_launch(&w,p,SKY_PARTICLES,NULL);
        unsigned colours=0;for(int i=0;i<SKY_PARTICLES;i++) colours|=1u<<p[i].color;
        assert(colours==7);
    }
}
static void check_fallback(const sky_recipe_t *recipe, uint32_t key) {
    sky_world_t w;sky_composer_t c;
    // Exhaust the search deliberately: include the whole rotated family, not only sampled candidate failures.
    sky_compose_begin(&c,&w,key,recipe);c.attempt=UINT8_MAX;
    assert(sky_compose_step(&c) && c.simple);validate(&w);
    assert(w.kind==(recipe->elements==2?SKY_POUR:SKY_PUZZLE));
    if(recipe->elements>=3) assert(w.count_stars==2 && w.count_holes>=1);
    int solved=solved_by(w,true), passive=solved_by(w,false);
    if(solved<0 || passive>=0) printf("Fallback seed=%u lesson=%d solution=%d passive=%d\n",key,recipe->elements,solved,passive);
    assert(solved>=0 && passive<0);
}
int main(int argc,char **argv) {
    int seeds=argc>1?atoi(argv[1]):40, total=0, failures=0, passive=0, simple=0, holes=0, max_slices=0;
    for(int skill=0;skill<=255;skill+=85) for(int lesson=1;lesson<=SKY_LESSONS;lesson++) for(int seed=1;seed<=seeds;seed++) {
        sky_recipe_t recipe={.elements=(uint8_t)lesson,.skill=(uint8_t)skill,.root=(uint8_t)(seed%5),.first=lesson==1,.from={90,255}};
        sky_world_t a,b;sky_composer_t c;
        uint32_t key=(uint32_t)(seed*977+lesson*43+skill);
        sky_compose_begin(&c,&a,key,&recipe);
        int slices=0;
        while(!sky_compose_step(&c)) { slices++; assert(slices<1000); }
        if(slices>max_slices) max_slices=slices;
        simple+=c.simple;holes+=a.count_holes;total++;
        sky_compose(&b,key,&recipe);
        assert(!memcmp(&a,&b,sizeof a)); validate(&a);
        if(solved_by(a,true)<0) { failures++; printf("Unsolvable seed=%u lesson=%d skill=%d stars=%d holes=%d fallback=%d\n",key,lesson,skill,a.count_stars,a.count_holes,c.simple); }
        if(solved_by(a,false)>=0) { passive++; printf("Passive seed=%u lesson=%d skill=%d stars=%d holes=%d\n",key,lesson,skill,a.count_stars,a.count_holes); }
        check_fallback(&recipe,key);
    }
    printf("Skies: %d worlds + %d forced fallbacks, %d unsolved, %d passive, %d fallbacks, %d holes, max %d compose slices\n",total,total,failures,passive,simple,holes,max_slices);
    assert(!failures && !passive);
    free_skies_have_no_targets();
    return 0;
}
