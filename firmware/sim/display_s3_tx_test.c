#include "display_s3_tx_test.h"
#include "display_s3_tx.h"
#include "esp_timer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static uint16_t expected[448][384], old_image[448][368];
static atomic_bool operation_entered,operation_done;
static uint16_t pattern(int x,int y) {return (uint16_t)((x*263u+y*977u)^((unsigned)x<<9)^0x95b7u);}
static void write_rect(int x0,int y0,int x1,int y1) {
    display_s3_begin(x0,y0,x1,y1);
    for(int y=y0;y<y1;y++)memcpy(display_s3_next_row(),&expected[y][x0],(size_t)(x1-x0)*2);
    display_s3_end();
}
static void verify_screen(void) {
    for(int y=0;y<448;y++)assert(!memcmp(mock_screen[y],expected[y],368*2));
}
static void *write_unread_band(void *unused) {
    (void)unused;atomic_store(&operation_entered,true);write_rect(0,440,16,448);atomic_store(&operation_done,true);return NULL;
}
static void *reset_image(void *unused) {
    (void)unused;atomic_store(&operation_entered,true);assert(display_s3_init());atomic_store(&operation_done,true);return NULL;
}
static void failures(void) {
    for(int fail=1;fail<=4;fail++) {
        mock_allocation_call=0;mock_allocation_fail=fail;assert(!display_s3_init() && !mock_held);
    }
    mock_allocation_fail=0;
    for(int fail=1;fail<=4;fail++) {
        mock_semaphore_call=0;mock_semaphore_fail=fail;assert(!display_s3_init() && !mock_held);
    }
    mock_semaphore_fail=0;mock_task_fail=true;assert(!display_s3_init() && !mock_held);mock_task_fail=false;
    mock_allocation_call=0;mock_task_calls=0;assert(display_s3_init() && mock_held==4 && mock_allocation_call==4 && mock_task_calls==1);
}
static void touch(void) {
    for(int y=-1;y<=448;y++)for(int x=-1;x<=368;x++) {
        int16_t lx=-7,ly=-9;bool inside=x>=0 && x<368 && y>=0 && y<448;
        assert(display_s3_touch(x,y,&lx,&ly)==inside);
        if(!inside){assert(lx==-7 && ly==-9);continue;}
        assert(lx==(int)((x+.5)*480./368.) && ly==(int)((y+.5)*584./448.));
    }
}
static void two_completions(const display_s3_profile_t *first) {
    display_s3_profile_t p;uint32_t sequence;
    assert(display_s3_tx_last_completed(&p,&sequence) && sequence==2 && p.windows==2 && p.input_pixels==129);
    assert(p.sender_core==0);assert(p.sender_stack_free==2048);
    display_s3_tx_bursts_t bursts=display_s3_tx_take_bursts();
    assert(bursts.count==2 && bursts.sum_us==first->burst_us+p.burst_us);
    assert(bursts.max_us==(first->burst_us>p.burst_us?first->burst_us:p.burst_us));
    assert(!display_s3_tx_take_bursts().count);
}
static void asynchronous_credits(void) {
    display_s3_profile_t p;uint32_t sequence;assert(!display_s3_tx_last_completed(&p,&sequence));
    for(int y=0;y<448;y++)for(int x=0;x<384;x++)expected[y][x]=pattern(x,y);
    write_rect(0,0,384,448);for(int y=0;y<448;y++)memcpy(old_image[y],expected[y],368*2);
    mock_hold(true,true);int64_t started=display_s3_present();assert(started && started==mock_first_pixel);
    mock_wait_blocked();assert(!display_s3_tx_last_completed(&p,&sequence));
    int syncs=mock_syncs;assert(!display_s3_present() && mock_syncs==syncs); // no job accumulation
    expected[0][0]=0xF800;write_rect(0,0,1,1); // old first band already copied, DMA still outstanding
    for(int y=440;y<448;y++)for(int x=0;x<16;x++)expected[y][x]=0x7BE7;
    atomic_store(&operation_entered,false);atomic_store(&operation_done,false);
    pthread_t writer;atomic_store(&mock_credit_waiting,false);assert(!pthread_create(&writer,NULL,write_unread_band,NULL));mock_wait_credit();assert(!atomic_load(&operation_done));
    mock_release();pthread_join(writer,NULL);assert(atomic_load(&operation_done));
    display_s3_tx_wait_idle();assert(mock_windows==1 && mock_writes==56);
    assert(!memcmp(mock_screen,old_image,sizeof old_image)); // main writes never changed the immutable old job
    assert(display_s3_tx_last_completed(&p,&sequence) && sequence==1 && p.windows==1);
    assert(p.input_pixels==384*448 && p.native_pixels==368*448 && p.burst_us>0);
    display_s3_profile_t first=p;
    assert(display_s3_present());display_s3_tx_wait_idle();verify_screen();
    two_completions(&first);
    puts("TX: START precedes DONE; unread bands block, copied bands allow writes, DMA/old job pixels immutable, completion profiles/aggregate exact");
}
static void immutable_hulls(void) {
    for(int y=0;y<24;y++)for(int x=0;x<16;x++)expected[y][x]=0x1539;
    for(int y=440;y<448;y++)for(int x=352;x<368;x++)expected[y][x]=0x263A;
    write_rect(0,0,16,24);write_rect(352,440,368,448);
    for(int y=0;y<448;y++)memcpy(old_image[y],expected[y],368*2);
    mock_hold(true,true);assert(display_s3_present());mock_wait_blocked();
    expected[1][17]=0xF123;write_rect(17,1,18,2);
    for(int y=440;y<448;y++)for(int x=0;x<16;x++)expected[y][x]=0x53A9;
    atomic_store(&operation_done,false);atomic_store(&mock_credit_waiting,false);
    pthread_t writer;assert(!pthread_create(&writer,NULL,write_unread_band,NULL));mock_wait_credit();
    assert(!atomic_load(&operation_done));mock_release_queue();pthread_join(writer,NULL);
    display_s3_profile_t p;uint32_t sequence;
    assert(display_s3_tx_last_completed(&p,&sequence) && sequence==2);
    mock_release();display_s3_tx_wait_idle();assert(!memcmp(mock_screen,old_image,sizeof old_image));
    assert(display_s3_tx_last_completed(&p,&sequence) && sequence==3 && p.windows==2 && p.input_pixels==512);
    assert(display_s3_present());display_s3_tx_wait_idle();verify_screen();
    assert(display_s3_tx_take_bursts().count==2);
    puts("TX: read credit releases before DONE; main damage changes cannot alter a later old-job window");
}
static void padding_and_reset(void) {
    display_s3_profile_t prior;uint32_t prior_sequence;assert(display_s3_tx_last_completed(&prior,&prior_sequence));
    int windows=mock_windows,syncs=mock_syncs;
    for(int y=0;y<448;y++)for(int x=368;x<384;x++)expected[y][x]=0xFA75;
    write_rect(368,0,384,448);assert(!display_s3_present() && mock_windows==windows && mock_syncs==syncs);verify_screen();
    expected[20][20]=0x3277;write_rect(20,20,21,21);
    mock_hold(false,true);assert(display_s3_present());mock_wait_blocked();
    atomic_store(&operation_entered,false);atomic_store(&operation_done,false);
    pthread_t reset;atomic_store(&mock_done_waiting,false);assert(!pthread_create(&reset,NULL,reset_image,NULL));mock_wait_done();assert(!atomic_load(&operation_done));
    mock_release();pthread_join(reset,NULL);assert(atomic_load(&operation_done) && mock_task_calls==1 && mock_held==4);
    for(int y=0;y<448;y++)for(int x=0;x<384;x++)expected[y][x]=0;
    memset(mock_screen,0,sizeof mock_screen);expected[447][367]=0x95B7;write_rect(367,447,368,448);
    assert(display_s3_present());display_s3_tx_wait_idle();verify_screen();
    display_s3_profile_t p;uint32_t sequence;assert(display_s3_tx_last_completed(&p,&sequence) && sequence==prior_sequence+2);
    puts("TX: padding/no-damage keep panel unchanged; reinit drains old work before clearing one canonical image and reuses one task");
}
static void rejected_wait(const char *mode) {
    assert(display_s3_init());
    for(int y=0;y<448;y++)for(int x=0;x<384;x++)expected[y][x]=pattern(x,y);
    write_rect(0,0,384,448);
    if(!strcmp(mode,"start")){mock_suppress_start=true;display_s3_present();}
    else {
        mock_hold(true,true);assert(display_s3_present());mock_wait_blocked();
        if(!strcmp(mode,"credit")){display_s3_begin(0,440,16,448);display_s3_next_row();}
        else display_s3_tx_wait_idle();
    }
    assert(!"A required TX wait timed out without failing explicitly");
}
int main(int argc,char **argv) {
    if(argc==2){rejected_wait(argv[1]);return 0;}
    touch();failures();asynchronous_credits();immutable_hulls();padding_and_reset();mock_shutdown();
    puts("S3 overlap: exact pixels, bounded credits, delayed DMA, one worker, failures/timeouts, coherent completion and touch passed");
}
