// The C6 alpha cache and early clipping preserve the original integer raster.
#include <assert.h>
#include <stdio.h>
#include "../main/render_text.c"
#include "../main/render_projection.h"

static void palettes(void) {
    const uint8_t *first=coverage_palette(173);
    for(unsigned v=0;v<16;v++)assert(first[v]==(v*173u*32u+1912u)/3825u);
    for (unsigned opacity=0;opacity<256;opacity++) {
        const uint8_t *palette=coverage_palette((uint8_t)opacity);
        for (unsigned v=0;v<16;v++)assert(palette[v]==(v*opacity*32u+1912u)/3825u);
    }
    for(unsigned pass=0;pass<3;pass++)for(unsigned i=0;i<29;i++) {
        unsigned opacity=(i*17+pass*83)%256;
        const uint8_t *palette=coverage_palette((uint8_t)opacity);
        for(unsigned v=0;v<16;v++)assert(palette[v]==(v*opacity*32u+1912u)/3825u);
    }
}
static void black_first_use(void) {
    // This must be the first black_palette call: its zero BSS entry is a valid palette.
    const uint8_t alpha[16]={0};const uint16_t *pixels=black_palette(0,0,false,alpha);
    for(unsigned v=0;v<16;v++)assert(pixels[v]==0);
}
static uint16_t black_reference(uint16_t color,unsigned opacity,unsigned coverage,bool swap) {
    unsigned a=(coverage*opacity*32u+1912u)/3825u;
    unsigned r=(((color>>11)&31)*a)>>5,g=(((color>>5)&63)*a)>>5,b=((color&31)*a)>>5;
    uint16_t value=(uint16_t)((r<<11)|(g<<5)|b);
    return swap?rgb565_bswap(value):value;
}
static void black_palettes(void) {
    const uint16_t colors[]={0,0xFFFF,0xF800,0x07E0,0x001F,0xFFE0,0xF81F,0x07FF,0x2945,0xA55A,0x7BEF};
    for(unsigned c=0;c<sizeof colors/sizeof *colors;c++)for(unsigned opacity=0;opacity<256;opacity++)
        for(int swap=0;swap<2;swap++) {
            const uint16_t *pixels=black_palette(colors[c],(uint8_t)opacity,swap,coverage_palette((uint8_t)opacity));
            for(unsigned v=0;v<16;v++)assert(pixels[v]==black_reference(colors[c],opacity,v,swap));
        }
    const unsigned boundary[][2]={{0,15},{1,15},{8,15},{127,1},{128,7},{128,15},{254,15},{255,14},{255,15}};
    for(unsigned color=0;color<=65535;color++)for(unsigned i=0;i<sizeof boundary/sizeof *boundary;i++)
        for(int swap=0;swap<2;swap++) {
            unsigned opacity=boundary[i][0],v=boundary[i][1];
            const uint16_t *pixels=black_palette((uint16_t)color,(uint8_t)opacity,swap,coverage_palette((uint8_t)opacity));
            assert(pixels[v]==black_reference((uint16_t)color,opacity,v,swap));
        }
    // Churn every key component, including a zero-valued first key and repeated reads.
    for(unsigned i=0;i<91;i++) {
        uint16_t color=colors[i%(sizeof colors/sizeof *colors)];uint8_t opacity=(uint8_t)(i*31);
        const uint8_t *alpha=coverage_palette(opacity);
        const uint16_t *a=black_palette(color,opacity,i%2,alpha);
        assert(a==black_palette(color,opacity,i%2,alpha));
        for(unsigned v=0;v<16;v++)assert(a[v]==black_reference(color,opacity,v,i%2));
    }
}
static void glyph_reference(const font_t *font,const font_glyph_t *glyph,uint16_t *out,
                            int x0,int y0,int width,int rows,int stride,int gx,int gy,
                            uint16_t color,uint8_t opacity,bool swap) {
    for(int y=y0;y<y0+rows;y++)for(int x=x0;x<x0+width;x++) {
        int sx=x-gx,sy=y-gy;
        if(sx<0 || sx>=glyph->w || sy<0 || sy>=glyph->h)continue;
        unsigned sample=(font->px[sy*((glyph->w+1)/2)+sx/2]>>((sx%2)*4))&15;
        if(!sample)continue;
        unsigned alpha=(sample*opacity*32u+1912u)/3825u;
        uint16_t *pixel=&out[(y-y0)*stride+x-x0];
        uint16_t old=swap?rgb565_bswap(*pixel):*pixel;
        uint16_t value=rgb565_blend(color,old,alpha);
        *pixel=swap?rgb565_bswap(value):value;
    }
}
static void black_glyph_case(unsigned opacity,bool swap) {
    enum {W=9,H=7,STRIDE=14,GUARD=8,X=3,Y=4};
    uint8_t samples[9*7];
    for(unsigned i=0;i<sizeof samples;i++)samples[i]=(uint8_t)(i*37);
    font_t font={.px=samples};font_glyph_t glyph={.w=13,.h=9};
    uint16_t actual[GUARD+H*STRIDE+GUARD],expected[GUARD+H*STRIDE+GUARD];
    {
        for(unsigned i=0;i<sizeof actual/sizeof *actual;i++)actual[i]=expected[i]=0xA55A;
        for(int y=0;y<H;y++)for(int x=0;x<W;x++) {
            uint16_t value=(x+y)%3?0:0x2945;
            actual[GUARD+y*STRIDE+x]=expected[GUARD+y*STRIDE+x]=swap?rgb565_bswap(value):value;
        }
        for(int overlap=0;overlap<2;overlap++) {
            uint16_t color=overlap?0xF124:0xA6FF;uint8_t alpha=(uint8_t)(opacity+overlap*73);
            const uint8_t *coverage=coverage_palette(alpha);
            const uint16_t *black=black_palette(color,alpha,swap,coverage);
            int gx=overlap?5:1,gy=overlap?6:3;
            render_text_glyph(&font,&glyph,actual+GUARD,X,Y,X+W-1,Y+H-1,STRIDE,
                              gx,gy,coverage,black,color,swap);
            glyph_reference(&font,&glyph,expected+GUARD,X,Y,W,H,STRIDE,gx,gy,color,alpha,swap);
            assert(!memcmp(actual,expected,sizeof actual));
        }
    }
}

static void black_glyphs(void) {
    for(unsigned opacity=0;opacity<256;opacity++)for(int swap=0;swap<2;swap++)black_glyph_case(opacity,swap);
}

static void interval_bounds(void) {
    for(int left=-33;left<34;left++)for(int size=1;size<66;size++)
        for(int x0=-3;x0<4;x0++)for(int width=1;width<4;width++) {
            int xs=imax(x0,icon_floor_q4(left));
            int xe=imin(x0+width-1,icon_ceil_q4(left+size)-1);
            assert(icon_window_outside(left,left,size,x0,x0,width,width)==(xs>xe));
        }
}
static int reference_axis(int pixel,int origin,int size) {
    int q=((pixel*16+8-origin)*ICON_MASK_SIZE*256)/size-128;
    if(q<0)return 0;
    int end=(ICON_MASK_SIZE-1)*256;
    return q>end?end:q;
}
static void column_coordinates(void) {
    uint16_t out[UI_W + 2];
    const int sizes[]={1,15,16,17,31,127,255,512,767,1024,7680,32767};
    for(unsigned n=0;n<sizeof sizes/sizeof *sizes;n++)
        for(int origin=-79;origin<100;origin++)for(int xs=-3;xs<UI_W;xs+=29) {
            int count=UI_W-xs; if(count>UI_W)count=UI_W;
            out[0]=out[count+1]=0xA55A;
            icon_columns(out+1,xs,count,origin,sizes[n]);
            for(int x=0;x<count;x++)assert(out[x+1]==reference_axis(xs+x,origin,sizes[n]));
            assert(out[0]==0xA55A && out[count+1]==0xA55A);
        }
    puts("icon columns: exact scalar division coordinates, signed fractional origins, clipped spans and canaries");
}
static unsigned mask_pixel(const uint8_t *mask,int x,int y) {
    int i=y*ICON_MASK_SIZE+x;
    return (mask[i/2]>>((i%2)?0:4))&15;
}
static unsigned reference_coverage(const prim_t *p,int x,int y) {
    int left=p->sub_x-p->a/2,top=p->sub_y-p->a/2;
    int qx=reference_axis(x,left,p->a),qy=reference_axis(y,top,p->a);
    int xa=qx/256,ya=qy/256,xb=xa+1<ICON_MASK_SIZE?xa+1:xa,yb=ya+1<ICON_MASK_SIZE?ya+1:ya;
    int fx=qx%256,fy=qy%256;const uint8_t *mask=g_icon_masks[p->b];
    unsigned a=mask_pixel(mask,xa,ya)*(256-fx)+mask_pixel(mask,xb,ya)*fx;
    unsigned b=mask_pixel(mask,xa,yb)*(256-fx)+mask_pixel(mask,xb,yb)*fx;
    return (a*(256-fy)+b*fy+32768u)/65536u;
}
static uint16_t reference_pixel(const prim_t *p,int x,int y,bool swap) {
    const uint16_t bg=0x2945;uint16_t value=bg;
    int left=p->sub_x-p->a/2,top=p->sub_y-p->a/2;
    if(x>=icon_floor_q4(left) && x<icon_ceil_q4(left+p->a) &&
       y>=icon_floor_q4(top) && y<icon_ceil_q4(top+p->a)) {
        unsigned coverage=reference_coverage(p,x,y);
        unsigned alpha=(coverage*p->alpha*32u+1912u)/3825u;
        value=rgb565_blend(p->color,bg,alpha);
    }
    return swap?rgb565_bswap(value):value;
}
static void clipped_case(int offset,int size,uint8_t alpha,bool swap) {
    enum {X=8,Y=12,W=16,H=12,STRIDE=22};
    uint16_t out[8+H*STRIDE+8];
    for(unsigned i=0;i<sizeof out/sizeof *out;i++)out[i]=0xA55A;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)out[8+y*STRIDE+x]=swap?rgb565_bswap(0x2945):0x2945;
    prim_t p={.kind=PK_ICON,.b=ICON_MIC,.a=size,.sub_x=X*16+offset,.sub_y=Y*16+offset,.color=0xFFFF,.alpha=alpha};
    render_icon_primitive(&p,out+8,X,Y,W,H,STRIDE,swap);
    for(int y=0;y<H;y++)for(int x=0;x<STRIDE;x++)
        assert(out[8+y*STRIDE+x]==(x<W?reference_pixel(&p,X+x,Y+y,swap):0xA55A));
    for(int i=0;i<8;i++){assert(out[i]==0xA55A);assert(out[8+H*STRIDE+i]==0xA55A);}
}
static void clipped_icons(void) {
    const int offsets[]={-193,-17,-1,0,1,7,15,17,257};
    const int sizes[]={1,16,35,80,287,576,773};
    const uint8_t alphas[]={0,1,17,64,128,254,255};
    for(unsigned a=0;a<sizeof alphas;a++)for(unsigned o=0;o<sizeof offsets/sizeof *offsets;o++)
        for(unsigned s=0;s<sizeof sizes/sizeof *sizes;s++)for(int swap=0;swap<2;swap++)
            clipped_case(offsets[o],sizes[s],alphas[a],swap);
}
static void full_width_icon(void) {
    enum {W=UI_W,H=4,STRIDE=W+8,Y=200};
    static uint16_t out[8+H*STRIDE+8];
    prim_t p={.kind=PK_ICON,.b=ICON_MIC,.a=UI_W*16,.sub_x=UI_W*8-1,
              .sub_y=UI_W*8-1,.color=0xFFFF,.alpha=173};
    for(int swap=0;swap<2;swap++) {
        for(unsigned i=0;i<sizeof out/sizeof *out;i++)out[i]=0xA55A;
        for(int y=0;y<H;y++)for(int x=0;x<W;x++)out[8+y*STRIDE+x]=swap?rgb565_bswap(0x2945):0x2945;
        render_icon_primitive(&p,out+8,0,Y,W,H,STRIDE,swap);
        for(int y=0;y<H;y++)for(int x=0;x<STRIDE;x++)
            assert(out[8+y*STRIDE+x]==(x<W?reference_pixel(&p,x,Y+y,swap):0xA55A));
        for(int i=0;i<8;i++){assert(out[i]==0xA55A);assert(out[8+H*STRIDE+i]==0xA55A);}
    }
}
static void text_window(const scene_t *scene,const prim_t *p,int x0,int width,bool swap) {
    enum {H=96,STRIDE=32,Y=24};
    uint16_t actual[H*STRIDE],reference[H*STRIDE];
    for(unsigned i=0;i<H*STRIDE;i++)actual[i]=reference[i]=0x2945;
    prim_t unculled=*p;
    unculled.bx0=0;unculled.bx1=UI_W-1;
    render_text_primitive(scene,&unculled,reference,x0,Y,width,H,STRIDE,swap);
    render_text_primitive(scene,p,actual,x0,Y,width,H,STRIDE,swap);
    assert(!memcmp(actual,reference,sizeof actual));
}
static void text_boundaries(void) {
    static scene_t scene;
    _Static_assert(sizeof scene.text==R_TEXT_POOL,"Text capacity uses one byte per token");
    for(unsigned f=0;f<FONT_COUNT;f++)for(int projected=0;projected<2;projected++) {
        scene_begin(&scene,0);
        scene.text[0]=(uint8_t)font_glyph(&g_fonts[f],'W');
        scene.text[1]=R_EMOJI_PLACEHOLDER;
        scene.text[2]=(uint8_t)font_glyph(&g_fonts[f],'j');
        scene.text[3]=(uint8_t)font_glyph(&g_fonts[f],'V');
        scene.text_n=4;
        prim_t *p=render_make_text(&scene,&g_fonts[f],(int)f,0,4,48,80,-1,0,0xFFFFFF,.73f);
        // This private C6 shader sees projected bounds on the actual C6 path.
#if !CONFIG_IDF_TARGET_ESP32S3
        if(projected)render_project_scene(&scene);
#endif
        int scale=scene.raster_ready?R_SCALE:1;
        int edges[]={p->bx0*scale,(p->bx1+1)*scale};
        for(unsigned edge=0;edge<2;edge++)for(int dx=-3;dx<=3;dx++)
            for(int width=1;width<=17;width+=8)for(int swap=0;swap<2;swap++) {
                int x=edges[edge]+dx;
                if(x>=0 && x+width<=UI_W)text_window(&scene,p,x,width,swap);
            }
        scene_end(&scene);
    }
}
static void kern_pairs(void) {
    static scene_t scene;
    unsigned negatives=0,zeros=0;
    for(unsigned f=0;f<FONT_COUNT;f++) {
        assert(g_fonts[f].n<=R_EMOJI_TOKEN);
        for(unsigned a=0;a<g_fonts[f].n+2;a++)for(unsigned b=0;b<g_fonts[f].n+2;b++) {
            unsigned first=a<g_fonts[f].n ? a : R_EMOJI_TOKEN+a-g_fonts[f].n;
            unsigned second=b<g_fonts[f].n ? b : R_EMOJI_TOKEN+b-g_fonts[f].n;
            int expected=first<R_EMOJI_TOKEN && second<R_EMOJI_TOKEN ? font_kern(&g_fonts[f],first,second) : 0;
            negatives+=expected<0;zeros+=expected==0;
            scene_begin(&scene,0);
            scene.text[1]=(uint8_t)first;scene.text[2]=(uint8_t)second;scene.text_n=3;
            memset(scene.text_kern,0x6b,4);
            render_make_text(&scene,&g_fonts[f],(int)f,1,2,30,64,-1,0,0xFFFFFF,1);
            assert(scene.text_kern[0]==0x6b && scene.text_kern[3]==0x6b);
            assert(scene.text_kern[1]==0 && scene.text_kern[2]==expected);
            scene_end(&scene);
        }
    }
    assert(negatives && zeros);
}
// Prior pen layout computes each pair directly; it does not use the scene's cached deltas.
static void reference_text(const scene_t *scene,const prim_t *p,uint16_t *out,int width,int rows,int stride,bool swap) {
    static scene_t single;
    const font_t *font=&g_fonts[p->r];int pen=p->sub_x*16;
    scene_clone(&single,scene);single.n=1;
    for(int i=0;i<p->b;i++) {
        uint16_t token=scene->text[p->a+i];
        single.p[0]=*p;single.p[0].a=p->a+i;single.p[0].b=1;single.p[0].sub_x=(pen+8)>>4;
        single.text_kern[p->a+i]=0;
        render_text_overlay(&single,out,0,0,width,rows,stride,swap);
        pen+=render_token_advance(font,token)+p->sub_r+
             (i+1<p->b ? render_token_kern(font,token,scene->text[p->a+i+1]) : 0);
    }
    scene_end(&single);
}
static void kern_pixels(void) {
    enum {W=128,H=96,STRIDE=136};
    static scene_t scene;static uint16_t actual[H*STRIDE],reference[H*STRIDE];
    const int chunks[]={4,8,H};
    const unsigned codepoints[]={'A','V',0,'T','o',' ','W','A','V',' ','j'};
    for(unsigned f=0;f<FONT_COUNT;f++)for(int swap=0;swap<2;swap++) {
        scene_begin(&scene,0);
        for(unsigned i=0;i<sizeof codepoints/sizeof *codepoints;i++)
            scene.text[scene.text_n++]=(uint8_t)(codepoints[i] ? font_glyph(&g_fonts[f],codepoints[i]) : R_EMOJI_PLACEHOLDER);
        prim_t *p=render_make_text(&scene,&g_fonts[f],(int)f,0,scene.text_n,3,64,-1,.3f,0xEFA3CF,.73f);
        for(unsigned c=0;c<sizeof chunks/sizeof *chunks;c++) {
            for(unsigned i=0;i<H*STRIDE;i++)actual[i]=reference[i]=0x2945;
            reference_text(&scene,p,reference,W,H,STRIDE,swap);
            for(int y=0;y<H;y+=chunks[c])
                render_text_overlay(&scene,actual+y*STRIDE,0,y,W,chunks[c],STRIDE,swap);
            assert(!memcmp(actual,reference,sizeof actual));
        }
        scene_end(&scene);
    }
}
static void kern_clones(void) {
    static scene_t scene,copy;
    scene_begin(&scene,0);
    sc_text(&scene,FONT_TITLE,"AV To",0,64,-1,0,0xFFFFFF,1);
    sc_text(&scene,FONT_CARD,"WAVE",0,80,-1,0,0xFFFFFF,1);
    scene_clone(&copy,&scene);
    int count=copy.text_n;
    scene_end(&scene);scene_begin(&scene,0);
    memset(scene.text_kern,0x6b,sizeof scene.text_kern);
    sc_text(&scene,FONT_CARD,"To",0,64,-1,0,0xFFFFFF,1);
    assert(scene.text_kern[0]==0);
    assert(scene.text_kern[1]==font_kern(&g_fonts[FONT_CARD],scene.text[0],scene.text[1]));
    for(int n=0;n<copy.n;n++) {
        const prim_t *p=&copy.p[n];
        assert(copy.text_kern[p->a]==0);
        for(int i=1;i<p->b;i++)assert(copy.text_kern[p->a+i]==font_kern(&g_fonts[p->r],copy.text[p->a+i-1],copy.text[p->a+i]));
    }
    assert(copy.text_n==count);
    scene_end(&scene);scene_end(&copy);
}
int main(void) {
    black_first_use();palettes();black_palettes();black_glyphs();interval_bounds();column_coordinates();clipped_icons();full_width_icon();text_boundaries();kern_pairs();kern_pixels();kern_clones();
    puts("black palette: all opacity/coverage, all RGB565 colors at boundaries, key churn, clipped/overlapping glyphs and stride guards exact");
    puts("scene kerning: every font pair/emoji, signed/zero deltas, prefix guards, old scalar pen pixels, split windows and clone/rebuild exact");
    puts("overlay alpha: all 4096 opacity/coverage values, eviction churn, fractional clipping and stride guards match original scalar math");
}
