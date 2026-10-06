// Real pack: routes must terminate, use its recorded pose links, and settle in requested states.
#include <assert.h>
#include <stdlib.h>
#include "../main/face_body.c"
static uint8_t *pack;
static size_t pack_size;
static const uint8_t *fetch(uint32_t o,uint32_t n,uint32_t ho,uint32_t hl) {
    (void)ho; (void)hl; return o<=pack_size && n<=pack_size-o?pack+o:NULL;
}
int main(void) {
    FILE *fp=fopen("firmware/assets/sprites.bin","rb"); assert(fp);
    fseek(fp,0,SEEK_END); pack_size=ftell(fp); rewind(fp);
    pack=malloc(pack_size); assert(fread(pack,1,pack_size,fp)==pack_size); fclose(fp);
    assert(sprite_init(fetch)); face_t f; face_init(&f);
    unsigned indirect=0,max_hops=0;
    for(int a=0;a<BA_COUNT;a++) for(int b=0;b<BA_COUNT;b++) {
        int node=a; unsigned hops=0;
        while(node!=b) { node=body_next[node][b]; assert(node>=0 && node<BA_COUNT); assert(++hops<BA_COUNT); }
        if(hops>1) indirect++; if(hops>max_hops) max_hops=hops;
    }
    assert(indirect>0);
    const face_mode_t modes[]={MODE_LISTENING,MODE_THINKING,MODE_SPEAKING,MODE_SLEEP};
    for(int a=0;a<BA_COUNT;a++) for(unsigned m=0;m<sizeof modes/sizeof modes[0];m++) {
        face_init(&f); f.body.req=-1;
        body_start(&f,a,false); f.body.pos=sprite_anim(f.body.id[a])->count*.5f;
        face_set_mode(&f,modes[m]);
        for(int i=0;i<20*R_FPS;i++) face_update(&f,1.f/R_FPS);
        assert(f.body.cur==body_want(&f));
    }
    printf("pose graph: %u indirect routes, max %u hops; all 52 state transitions reach their destination\n",indirect,max_hops);
    free(pack);
}
