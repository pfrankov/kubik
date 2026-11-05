// Execute the actual task-creation expressions with scheduler records instead of RTOS tasks.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "task_placement.h"
_Static_assert(KUBIK_UI_CORE==EXPECTED_UI_CORE,"UI core contract");
_Static_assert(KUBIK_AUDIO_CORE==0,"audio remains on core zero");
typedef void *TaskHandle_t;
typedef uint32_t StackType_t;
typedef int StaticTask_t;
static TaskHandle_t s_app_task,s_spk_task,s_mic_task,s_input_task,s_display_task;
static int s_mic_task_config;
static StackType_t stack_storage[4096/sizeof(StackType_t)],*s_stack=stack_storage;
static StaticTask_t s_task_storage;
#if CONFIG_IDF_TARGET_ESP32S3
static StackType_t *sender_memory;
static StaticTask_t *sender_storage;
#endif
/* PRODUCTION_STACK_SIZES */
/* TASK_CALLBACKS */
typedef struct {const char *name;unsigned stack,priority;int core;bool fixed_storage;} record_t;
static record_t records[16];
static unsigned created;
static void record(const char *name,unsigned stack,unsigned priority,int core,bool fixed_storage) {
    assert(created<16);records[created++]=(record_t){name,stack,priority,core,fixed_storage};
}
static int xTaskCreatePinnedToCore(void (*callback)(void *),const char *name,unsigned stack,
                                 void *context,unsigned priority,TaskHandle_t *handle,int core) {
    (void)context;assert(callback);record(name,stack,priority,core,false);
    if(handle)*handle=(void *)(uintptr_t)created;
    return 1;
}
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*callback)(void *),const char *name,unsigned stack,
                                                void *context,unsigned priority,StackType_t *memory,
                                                StaticTask_t *storage,int core) {
    (void)context;assert(callback);
#if CONFIG_IDF_TARGET_ESP32S3
    if(!strcmp(name,"s3_spi")) {
        assert(memory==sender_memory && storage==sender_storage && memory!=s_stack && storage!=&s_task_storage);
        assert(stack==4096 && priority==23 && core==0);
    } else
#endif
    {
        assert(!strcmp(name,"mic_send") && memory==s_stack && storage==&s_task_storage);
        assert((stack==4096 && priority==7) || (stack==3072 && priority==6));
    }
    record(name,stack,priority,core,true);return (void *)(uintptr_t)created;
}
int main(void) {
    for(unsigned mode=0;mode<2;mode++) {
        bool duplex=mode!=0;created=0;
        /* PRODUCTION_CREATIONS */
        const record_t expected[]={/* EXPECTED_RECORDS */};
        assert(created==sizeof expected/sizeof expected[0]);
        assert(created==(CONFIG_IDF_TARGET_ESP32S3 ? 13u : 12u));
        for(unsigned i=0;i<created;i++) {
            assert(!strcmp(records[i].name,expected[i].name));
            assert(records[i].stack==expected[i].stack && records[i].priority==expected[i].priority);
            assert(records[i].core==expected[i].core && records[i].fixed_storage==expected[i].fixed_storage);
        }
    }
    puts("task placement: actual creation calls, callback arguments, stack sizes, priorities, UI/audio/SPI cores, static SPI storage and both mic sender modes passed");
}
