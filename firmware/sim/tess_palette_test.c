// The prior arithmetic is a scalar oracle; cached coefficients must preserve every shade.
#include <assert.h>
#include <stdio.h>
#include "../main/tess_draw.c"
static uint32_t reference_mood_color(const face_t *f, uint32_t c, float d) {
    float total = 1;
    for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) total -= f->feel.mix[mood];
    total = fmaxf(0.f, total);  // (the weights are rounded: they may add up to a little over one)
    for (int mood = MOOD_CURIOUS; mood < MOOD_COUNT; mood++) {
        float weight = f->feel.mix[mood];
        if (weight <= 0) continue;
        total += weight;
        c = rgb_mix(c, ramp(k_mood_ramp[mood][0], k_mood_ramp[mood][1], k_mood_ramp[mood][2], d), weight / total);
    }
    return c;
}

static uint32_t reference_color(const face_t *f, float d) {
    const float *r = f->tess_reaction, *mw = f->tess_mode;
    uint32_t c = reference_mood_color(f, tess_depth_ramp(d), d);
    if (f->tess_mood.warm > .01f) c = rgb_mix(c, ramp(0x1C0E06, 0xB8641A, 0xFFC868, d), tess_warmth(f) * .9f);
    if (r[TR_SAD] > .01f) c = rgb_mix(c, ramp(0x030826, 0x1E3E9A, 0x5A86F0, d), r[TR_SAD] * .8f);
    if (r[TR_ANGRY] > .01f) c = rgb_mix(c, ramp(0x200605, 0x9A2A1C, 0xFF7152, d), r[TR_ANGRY]);
    if (r[TR_JOY] > .01f) c = rgb_mix(c, ramp(0x160E04, 0x8C6418, 0xFFCB5C, d), r[TR_JOY] * .7f);
    if (r[TR_SHY] > .01f) c = rgb_mix(c, ramp(0x200A1A, 0x93406E, 0xFF9CCB, d), r[TR_SHY] * .7f);
    if (r[TR_HEART] > .01f) c = rgb_mix(c, ramp(0x2E0B28, 0xA3316F, 0xFF8AC4, d), r[TR_HEART]);
    float reply = reply_tint(f);
    if (reply > 0) c = rgb_mix(c, ramp(0x120825, 0x2BA69A, 0xFFE2A0, d), reply);
    float play = game_tint(f);
    if (play > 0) c = rgb_mix(c, ramp(0x170D04, 0x987524, 0xFFCE78, d), play * .65f);
    // No connection: noir. Black and white, over any mood, at full contrast: dark grey specks far, pure white near.
    float noir = tess_noir(f);
    if (noir > .01f) c = rgb_mix(c, ramp(0x202020, 0xA6A6A6, 0xFFFFFF, d), noir);
    float dim = 1 - .45f * mw[TM_SLEEP];
    return dim < .99f ? rgb_scale(c, dim) : c;
}


static uint32_t rng=1;
static float value(void) { rng=rng*1664525u+1013904223u; return (rng>>8)*(1.f/16777216.f); }
int main(void) {
    face_t face={0};
    for(int state=0;state<4000;state++) {
        for(int mood=0;mood<MOOD_COUNT;mood++)face.feel.mix[mood]=state%3==0?0:value();
        if(state>=2000) {
            static const float tiny[]={0,1e-20f,1e-8f,1.f/1024,1.f/256,0x1.000002p-8f,0x1.fffffep-9f,.2f};
            for(int mood=0;mood<MOOD_COUNT;mood++)face.feel.mix[mood]=tiny[(state+mood)%8];
        }
        for(int k=0;k<TR_COUNT;k++)face.tess_reaction[k]=state%5==0?0:value();
        for(int k=0;k<TM_COUNT;k++)face.tess_mode[k]=value();
        face.tess_mood.warm=value();face.tess_audio=value();face.tess_fallen=state%7==0;
        face.tess_games.ready=state%2;face.tess_games.pulse=value();
        palette_t palette=prepare_palette(&face);
        for(int i=0;i<TESS_SHADES;i++) {
            float d=(i+.5f)/TESS_SHADES;
            assert(palette_color(&face,d,&palette)==reference_color(&face,d));
        }
    }
    puts("palette: 128000 shades, including tiny weights and exact no-op boundary, byte-exact against prior mood/reaction/tint arithmetic");
}
