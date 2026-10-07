#!/usr/bin/env python3
"""Run the actual bounded mic delivery worker with pthread-backed RTOS primitives."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = '\n'.join(line for line in (ROOT / 'firmware/main/mic_delivery.c').read_text().splitlines()
                   if not line.startswith('#include'))
fixture = r'''
#include <assert.h>
#include <pthread.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mic_delivery.h"
#include "ima_adpcm.h"
typedef struct { pthread_mutex_t lock; pthread_cond_t changed; unsigned count; } sem_t;
static int sem_init(sem_t *s,int shared,unsigned value) {
    (void)shared; pthread_mutex_init(&s->lock,NULL); pthread_cond_init(&s->changed,NULL); s->count=value; return 0;
}
static int sem_post(sem_t *s) {
    pthread_mutex_lock(&s->lock); s->count++; pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock); return 0;
}
static int sem_wait(sem_t *s) {
    pthread_mutex_lock(&s->lock); while(!s->count)pthread_cond_wait(&s->changed,&s->lock);
    s->count--; pthread_mutex_unlock(&s->lock); return 0;
}
static void sem_destroy(sem_t *s) {pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->lock);}
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_RTCRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
typedef uint8_t StackType_t;
typedef struct { pthread_t thread; sem_t wake; void (*fn)(void *); void *arg; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
typedef sem_t StaticSemaphore_t;
typedef sem_t *SemaphoreHandle_t;
static TaskHandle_t current;
static void *entry(void *p) { TaskHandle_t t=p; t->fn(t->arg); return NULL; }
static TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned bytes,
    void *arg, int priority, StackType_t *stack, StaticTask_t *task) {
    assert(((bytes==4096 && priority==7) || (bytes==3072 && priority==6)) && stack); (void)name;
    task->fn=fn; task->arg=arg; sem_init(&task->wake,0,0); current=task;
    assert(!pthread_create(&task->thread,NULL,entry,task)); return task;
}
static void xTaskNotifyGive(TaskHandle_t task) { sem_post(&task->wake); }
static void ulTaskNotifyTake(int clear, unsigned timeout) {
    (void)clear; (void)timeout; assert(!sem_wait(&current->wake));
}
static void vTaskSuspend(void *unused) { (void)unused; pthread_exit(NULL); }
static void vTaskDelete(TaskHandle_t task) { pthread_join(task->thread,NULL); sem_destroy(&task->wake); }
static bool done_ready;
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {
    if (done_ready) sem_destroy(s);
    sem_init(s,0,0); done_ready=true; return s;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { sem_post(s); }
static void xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout) { (void)timeout; assert(!sem_wait(s)); }
static int alloc_fail=-1, held;
static void *checked_malloc(size_t n) {
    if (alloc_fail==0) return NULL;
    if (alloc_fail>0) alloc_fail--;
    void *p=malloc(n); if(p) held++; return p;
}
static void checked_free(void *p) { if(p) held--; free(p); }
static void *heap_caps_malloc(size_t n, unsigned caps) {
    assert(((n==4096 || n==3072) && caps==(MALLOC_CAP_RTCRAM|MALLOC_CAP_8BIT)) || (n==15552 && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    return checked_malloc(n);
}
static void heap_caps_free(void *p) { checked_free(p); }
static void *workspace;
static void *tls_mem_capture_take(size_t n) {
    assert(n==16560); // Packed slots and decode scratch fit the existing TLS workspace.
    if(!workspace)workspace=checked_malloc(n);
    return workspace;
}
static void tls_mem_capture_release(void) {
    for(size_t i=0;i<16560;i++)assert(((unsigned char *)workspace)[i]==0);
    checked_free(workspace); workspace=NULL;
}
static void vTaskDelay(int ms) {(void)ms;}
#define pdMS_TO_TICKS(x) (x)
#define malloc checked_malloc
#define free checked_free
'''
tests = r'''
#undef malloc
#undef free
static sem_t entered, release, processed;
static bool hold_first, acknowledge;
static unsigned calls, errors;
static ima_state_t reference_encoder;
static void deliver(void *context, const int16_t *pcm, const uint8_t *ima, uint32_t epoch) {
    assert(context==&calls && epoch==42);
    if (!pcm) { assert(!ima); errors++; return; }
    int16_t original[960], expected_pcm[960]; uint8_t expected[483];
    for(int i=0;i<960;i++) original[i]=(int16_t)(calls*100+i*17);
    ima_encode(&reference_encoder,original,960,expected);
    assert(!memcmp(ima,expected,sizeof expected));
    ima_state_t decoder; assert(ima_read_header(expected,&decoder));
    ima_decode(&decoder,expected+3,480,expected_pcm);
    assert(!memcmp(pcm,expected_pcm,sizeof expected_pcm));
    calls++;
    if(hold_first && calls==1) {sem_post(&entered); assert(!sem_wait(&release));}
    if(acknowledge)sem_post(&processed);
}
static void push(unsigned n) {
    int16_t pcm[960]; for(int i=0;i<960;i++)pcm[i]=(int16_t)(n*100+i*17);
    mic_delivery_push(pcm,42,NULL); memset(pcm,0,sizeof pcm);
}
static void deliver_duplex(void *context,const int16_t *pcm,const uint8_t *ima,uint32_t epoch) {
    assert(context==&calls && !pcm && epoch==42);
    if (!ima) { errors++; return; }
    int16_t near[960], far[960]; ima_state_t decoder;
    assert(ima_read_header(ima,&decoder)); ima_decode(&decoder,ima+3,480,near);
    assert(ima_read_header(ima+483,&decoder)); ima_decode(&decoder,ima+486,480,far);
    assert(near[900]>900 && far[900]<-900); calls++;
    if(hold_first && calls==1) {sem_post(&entered); assert(!sem_wait(&release));}
}
static void duplex_proof(void) {
    calls=errors=0;
    for(int fail=0;fail<2;fail++) {
        alloc_fail=fail; assert(!mic_delivery_start(deliver_duplex,&calls,true)); assert(!held && !s_task);
    }
    alloc_fail=-1;
    assert(mic_delivery_start(deliver_duplex,&calls,true)); assert(!workspace && !s_pcm);
    int16_t near[960], far[960]; for(int i=0;i<960;i++){near[i]=1000;far[i]=-1000;}
    uint8_t reference[483]; ima_state_t ref_encoder={0}; ima_encode(&ref_encoder,far,960,reference);
    for(int i=0;i<16;i++)mic_delivery_push(near,42,reference);
    mic_delivery_stop(); assert(calls==16 && !errors && !held);
    assert(mic_delivery_start(deliver_duplex,&calls,true));
    mic_delivery_push(NULL,42,NULL); mic_delivery_stop(); assert(errors==1 && !held);
    calls=errors=0; hold_first=true;
    assert(mic_delivery_start(deliver_duplex,&calls,true));
    mic_delivery_push(near,42,reference); assert(!sem_wait(&entered));
    for(int i=1;i<17;i++)mic_delivery_push(near,42,reference);
    sem_post(&release); mic_delivery_stop(); assert(calls==1 && errors==1 && !held);
    hold_first=false;
}
int main(void) {
    sem_init(&entered,0,0); sem_init(&release,0,0); sem_init(&processed,0,0);
    for(int fail=0;fail<2;fail++) {
        alloc_fail=fail; assert(!mic_delivery_start(deliver,&calls,false)); assert(!held && !s_task);
    }
    alloc_fail=-1;
    assert(mic_delivery_start(deliver,&calls,false)); assert(!mic_delivery_start(deliver,&calls,false));
    for(unsigned i=0;i<FRAMES;i++)push(i);
    mic_delivery_stop(); assert(calls==FRAMES && errors==0 && held==0 && !s_task);
    mic_delivery_stop(); // idempotent cleanup
    calls=0; reference_encoder=(ima_state_t){0}; hold_first=true;
    assert(mic_delivery_start(deliver,&calls,false)); push(0); assert(!sem_wait(&entered));
    for(unsigned i=1;i<11;i++)push(i); // A >320ms blocked send must not lose the next PCM frames.
    sem_post(&release); mic_delivery_stop();
    assert(calls==11 && errors==0 && !held);
    calls=0; reference_encoder=(ima_state_t){0}; hold_first=true;
    assert(mic_delivery_start(deliver,&calls,false)); push(0); assert(!sem_wait(&entered));
    for(unsigned i=1;i<FRAMES+1;i++)push(i); // Bounded slots include the one being sent.
    sem_post(&release); mic_delivery_stop();
    assert(calls==1 && errors==1 && !held);
    calls=errors=0; reference_encoder=(ima_state_t){0}; hold_first=false;
    assert(mic_delivery_start(deliver,&calls,false)); push(0); mic_delivery_stop();
    assert(calls==1 && errors==0 && !held);
    calls=0; reference_encoder=(ima_state_t){0}; acknowledge=true;
    assert(mic_delivery_start(deliver,&calls,false));
    for(unsigned i=0;i<FRAMES*3;i++){push(i);assert(!sem_wait(&processed));}
    mic_delivery_stop(); assert(calls==FRAMES*3 && errors==0 && !held);
    assert(!s_pcm);
    duplex_proof();
    puts("mic delivery: ordered drain, copied PCM, bounded overflow abort, allocation cleanup, restart");
}
'''
with tempfile.TemporaryDirectory(prefix='c6-mic-delivery-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + source + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'firmware/main'), str(path / 'test.c'), str(ROOT / 'firmware/main/ima_adpcm.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
