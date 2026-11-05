#include "display_s3_tx_test.h"
#include "display_s3_tx.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
int mock_allocation_call, mock_allocation_fail, mock_held;
int mock_semaphore_call, mock_semaphore_fail, mock_task_calls;
bool mock_task_fail, mock_suppress_start;
atomic_bool mock_blocked, mock_credit_waiting, mock_done_waiting;
uint16_t mock_screen[448][368];
int mock_windows, mock_writes, mock_syncs;
int64_t mock_first_pixel;
static void *allocations[4];
static TaskHandle_t task;
static pthread_mutex_t raw_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t raw_changed=PTHREAD_COND_INITIALIZER;
static bool hold_queue,hold_done,raw_open;
static int raw_x,raw_y,raw_width,raw_end;
static void (*started)(void *);
static void *started_context;
static struct {const uint16_t *data;int x,y,width;uint32_t hash;} pending[2];
static int pending_count;
static _Thread_local bool capture_first_timer;
int xPortGetCoreID(void) {return 0;}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle) {(void)handle;return 2048;}
int64_t esp_timer_get_time(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);int64_t now=(int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
    if(capture_first_timer){mock_first_pixel=now;capture_first_timer=false;}return now;
}
uint32_t esp_cpu_get_cycle_count(void) {return (uint32_t)((uint64_t)esp_timer_get_time()*240);}
void mock_sleep(unsigned ms) {struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL);}
void *heap_caps_malloc(size_t bytes,unsigned caps) {
    mock_allocation_call++;
    assert((bytes==384*448*2 && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)) ||
           (bytes==368*8*2 && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT)));
    if(mock_allocation_call==mock_allocation_fail)return NULL;
    void *p=malloc(bytes);assert(p);
    for(int i=0;i<4;i++)if(!allocations[i]){allocations[i]=p;mock_held++;return p;}
    abort();
}
void *heap_caps_calloc(size_t count,size_t size,unsigned caps) {
    void *p=heap_caps_malloc(count*size,caps);if(p)memset(p,0,count*size);return p;
}
void heap_caps_free(void *p) {
    if(!p)return;
    for(int i=0;i<4;i++)if(allocations[i]==p){allocations[i]=NULL;mock_held--;free(p);return;}
    abort();
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {
    mock_semaphore_call++;if(mock_semaphore_call==mock_semaphore_fail)return NULL;
    assert(!s->initialized);pthread_mutex_init(&s->lock,NULL);pthread_cond_init(&s->ready,NULL);
    s->count=0;s->role=(mock_semaphore_call-1)%4;s->initialized=true;return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks) {
    pthread_mutex_lock(&s->lock);
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_sec+=ticks/1000;until.tv_nsec+=(long)(ticks%1000)*1000000;
    if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
    while(!s->count && ticks) {
        if(s->role==3)atomic_store(&mock_credit_waiting,true);
        if(s->role==2)atomic_store(&mock_done_waiting,true);
        int result=ticks==portMAX_DELAY?pthread_cond_wait(&s->ready,&s->lock):pthread_cond_timedwait(&s->ready,&s->lock,&until);
        if(result==ETIMEDOUT)break;assert(!result);
    }
    int got=s->count;if(got)s->count=0;pthread_mutex_unlock(&s->lock);return got?pdTRUE:pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    pthread_mutex_lock(&s->lock);s->count=1;pthread_cond_signal(&s->ready);pthread_mutex_unlock(&s->lock);return pdTRUE;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *woken) {*woken=pdFALSE;return xSemaphoreGive(s);}
void vSemaphoreDelete(SemaphoreHandle_t s) {assert(s->initialized);pthread_mutex_destroy(&s->lock);pthread_cond_destroy(&s->ready);s->initialized=false;}
static void *entry(void *arg) {StaticTask_t *s=arg;s->entry(s->context);return NULL;}
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *),const char *name,uint32_t stack,void *context,unsigned priority,
                                         StackType_t *storage,StaticTask_t *tcb,int core) {
    assert(!strcmp(name,"s3_spi") && stack==4096 && !context && priority==23 && storage && tcb && core==0);
    mock_task_calls++;if(mock_task_fail)return NULL;
    tcb->entry=fn;tcb->context=context;assert(!pthread_create(&tcb->thread,NULL,entry,tcb));task=tcb;return tcb;
}
static uint32_t checksum(const uint16_t *data,int n) {uint32_t h=2166136261u;for(int i=0;i<n;i++)h=(h^data[i])*16777619u;return h;}
static void unchanged(void) {
    for(int i=0;i<pending_count;i++)assert(checksum(pending[i].data,pending[i].width*8)==pending[i].hash);
}
static void complete_one(void) {
    unchanged();assert(pending_count);
    for(int row=0;row<8;row++)memcpy(&mock_screen[pending[0].y+row][pending[0].x],pending[0].data+row*pending[0].width,(size_t)pending[0].width*2);
    if(pending_count==2)pending[0]=pending[1];pending_count--;
}
void board_display_frame_start(void (*callback)(void *),void *context) {
    pthread_mutex_lock(&raw_lock);assert(!started && !raw_open && !pending_count);started=callback;started_context=context;pthread_mutex_unlock(&raw_lock);
}
void board_display_begin(int x0,int y0,int x1,int y1) {
    pthread_mutex_lock(&raw_lock);assert(!raw_open && x0>=0 && x1<=368 && x1>x0 && y0>=0 && y1<=448 && y1>y0);
    raw_x=x0;raw_y=y0;raw_width=x1-x0;raw_end=y1;raw_open=true;mock_windows++;pthread_mutex_unlock(&raw_lock);
}
void board_display_write(const uint16_t *data,int count,bool last) {
    pthread_mutex_lock(&raw_lock);assert(raw_open && count==raw_width*8);unchanged();
    for(int i=0;i<pending_count;i++)assert(data!=pending[i].data);
    while(pending_count==2 && hold_queue){atomic_store(&mock_blocked,true);pthread_cond_wait(&raw_changed,&raw_lock);}
    if(pending_count==2)complete_one();
    pending[pending_count].data=data;pending[pending_count].x=raw_x;pending[pending_count].y=raw_y;
    pending[pending_count].width=raw_width;pending[pending_count].hash=checksum(data,count);pending_count++;
    void (*callback)(void *)=started;void *context=started_context;
    if(callback && !mock_suppress_start){started=NULL;capture_first_timer=true;callback(context);}
    raw_y+=8;assert(last==(raw_y==raw_end));mock_writes++;pthread_mutex_unlock(&raw_lock);
}
void board_display_end(void) {pthread_mutex_lock(&raw_lock);assert(raw_open && raw_y==raw_end);raw_open=false;pthread_mutex_unlock(&raw_lock);}
void board_display_wait_idle(void) {
    pthread_mutex_lock(&raw_lock);
    while(hold_done){atomic_store(&mock_blocked,true);pthread_cond_wait(&raw_changed,&raw_lock);}
    while(pending_count)complete_one();assert(!raw_open);pthread_mutex_unlock(&raw_lock);
}
esp_err_t frame_pacing_s3_sync(void) {
    pthread_mutex_lock(&raw_lock);assert(!pending_count && !raw_open);mock_syncs++;pthread_mutex_unlock(&raw_lock);return ESP_OK;
}
void mock_hold(bool queue,bool done) {
    pthread_mutex_lock(&raw_lock);hold_queue=queue;hold_done=done;atomic_store(&mock_blocked,false);pthread_mutex_unlock(&raw_lock);
}
void mock_release_queue(void) {pthread_mutex_lock(&raw_lock);hold_queue=false;pthread_cond_broadcast(&raw_changed);pthread_mutex_unlock(&raw_lock);}
void mock_release(void) {pthread_mutex_lock(&raw_lock);hold_queue=hold_done=false;pthread_cond_broadcast(&raw_changed);pthread_mutex_unlock(&raw_lock);}
static void wait_flag(atomic_bool *flag) {
    int64_t deadline=esp_timer_get_time()+30000;
    while(!atomic_load(flag)){assert(esp_timer_get_time()<deadline);mock_sleep(1);}
}
void mock_wait_blocked(void) {wait_flag(&mock_blocked);}
void mock_wait_credit(void) {wait_flag(&mock_credit_waiting);}
void mock_wait_done(void) {wait_flag(&mock_done_waiting);}
void mock_shutdown(void) {
    pthread_cancel(task->thread);pthread_join(task->thread,NULL);
    for(int i=0;i<4;i++)if(allocations[i]){free(allocations[i]);allocations[i]=NULL;mock_held--;}
    assert(!mock_held);
}
