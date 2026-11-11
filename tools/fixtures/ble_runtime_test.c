#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NO_MEM -5
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_RTCRAM 8
#define ESP_LOGI(tag,format,a,b) ((void)(tag),(void)(format),(void)(a),(void)(b))
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096
#define CONFIG_BT_NIMBLE_PINNED_TO_CORE 0
#define configMAX_PRIORITIES 25
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
typedef struct { uint8_t type, val[6]; } ble_addr_t;
struct ble_hs_adv_fields;
struct os_mbuf;
typedef uint8_t StackType_t;
typedef struct { void (*entry)(void *); bool deleted, suspended; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
typedef struct { bool available; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static struct { void (*sync_cb)(void); } ble_hs_cfg;
static bool init_fails, prepare_fails, deinit_fails, host_fails, stop_hangs;
static unsigned allocations, frees, attempts, fail_attempt, init_calls, deinit_calls, stop_calls, syncs;
static uint32_t waited;
static TaskHandle_t fake_host, fake_stop;
static TaskHandle_t fake_current;
static unsigned join_delay, delayed_ticks;
static bool output_busy,reserved;
static unsigned reservations,releases;
static bool audio_ble_reserve(void) {
    assert(!reserved);if(output_busy)return false;reserved=true;reservations++;return true;
}
static void audio_ble_release(void) {
    assert(reserved && allocations==frees);reserved=false;releases++;
}
enum { eRunning, eSuspended };
static void *heap_caps_malloc(size_t n,int caps) {
    unsigned expected=CONFIG_IDF_TARGET_ESP32S3 ? MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT|MALLOC_CAP_DMA : MALLOC_CAP_RTCRAM|MALLOC_CAP_8BIT;
    assert(reserved && (unsigned)caps==expected);attempts++;
    if(fail_attempt==attempts)return NULL;
    allocations++;return malloc(n);
}
static void heap_caps_free(void *p) { if(p)frees++;free(p); }
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) { s->available=false;return s; }
static int xSemaphoreTake(SemaphoreHandle_t s,uint32_t ticks) {
    if(s->available){s->available=false;return 1;}waited+=ticks;return 0;
}
static void xSemaphoreGive(SemaphoreHandle_t s) { s->available=true; }
static void vTaskSuspend(TaskHandle_t task) { assert(!task&&fake_current);fake_current->suspended=true; }
static int eTaskGetState(TaskHandle_t task) { if(join_delay){join_delay--;return eRunning;}return task->suspended?eSuspended:eRunning; }
static void vTaskDelay(unsigned ticks) { assert(ticks==1);delayed_ticks++; }
static void vTaskDelete(TaskHandle_t task) { assert(task&&!task->deleted&&task->suspended);task->deleted=true; }
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { assert(task&&task->suspended);return 512; }
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void *),const char *name,unsigned size,
                                               void *argument,int priority,StackType_t *stack,StaticTask_t *storage,int core) {
    assert(!argument&&stack&&storage&&core==0);
    *storage=(StaticTask_t){.entry=entry};
    if(!strcmp(name,"nimble_host")){
        assert(size==4096&&priority==21);if(host_fails)return NULL;fake_host=storage;
    }else{
        assert(!strcmp(name,"ble_stop")&&size==3072&&priority==4);fake_stop=storage;
        if(!stop_hangs){fake_current=storage;entry(NULL);fake_current=NULL;}
    }
    return storage;
}
static int nimble_port_init(void) { assert(reserved);init_calls++;return init_fails?ESP_FAIL:ESP_OK; }
static int nimble_port_deinit(void) { deinit_calls++;return deinit_fails?ESP_FAIL:ESP_OK; }
static void nimble_port_run(void) {}
static int nimble_port_stop(void) { stop_calls++;TaskHandle_t prior=fake_current;fake_current=fake_host;fake_host->entry(NULL);fake_current=prior;return 0; }

#include "actual.inc"

static esp_err_t prepare(void) { return prepare_fails?ESP_FAIL:ESP_OK; }
static void synchronized(void) { syncs++; }
static void clean(void) {
    assert(ble_runtime_owned(BLE_RUNTIME_NONE));assert(!host_stack&&!stop_stack);
    assert(allocations==frees && !reserved && reservations==releases);
}

int main(void) {
    assert(ble_runtime_start(BLE_RUNTIME_NONE,prepare,synchronized)==ESP_ERR_INVALID_ARG);
    assert(ble_runtime_start(BLE_RUNTIME_PRINTER,NULL,synchronized)==ESP_ERR_INVALID_ARG);
    clean();
    output_busy=true;
    assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_ERR_INVALID_STATE);
    assert(!attempts && !init_calls);clean();output_busy=false;
    for(unsigned n=0;n<50;n++){
        assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_OK);
        ble_hs_cfg.sync_cb();assert(syncs==n+1);
        unsigned before=init_calls;
        assert(ble_runtime_start(BLE_RUNTIME_MUSE,prepare,synchronized)==ESP_ERR_INVALID_STATE);
        assert(init_calls==before);assert(ble_runtime_stop(BLE_RUNTIME_MUSE)==ESP_ERR_INVALID_STATE);
        assert(ble_runtime_stop(BLE_RUNTIME_PRINTER)==ESP_OK);clean();
    }
    assert(stop_calls==50&&deinit_calls==50);
    assert(ble_runtime_start(BLE_RUNTIME_MUSE,prepare,synchronized)==ESP_OK);
    assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_ERR_INVALID_STATE);
    assert(ble_runtime_stop(BLE_RUNTIME_MUSE)==ESP_OK);clean();
    fail_attempt=attempts+1;assert(ble_runtime_start(BLE_RUNTIME_MUSE,prepare,synchronized)==ESP_ERR_NO_MEM);clean();
    fail_attempt=attempts+2;assert(ble_runtime_start(BLE_RUNTIME_MUSE,prepare,synchronized)==ESP_ERR_NO_MEM);clean();fail_attempt=0;
    init_fails=true;assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_FAIL);clean();init_fails=false;
    prepare_fails=true;assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_FAIL);clean();prepare_fails=false;
    host_fails=true;assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_ERR_NO_MEM);clean();host_fails=false;
    assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_OK);stop_hangs=true;
    unsigned before=stop_calls;assert(ble_runtime_stop(BLE_RUNTIME_PRINTER)==ESP_ERR_TIMEOUT);
    assert(waited==8000&&stop_calls==before&&ble_runtime_owned(BLE_RUNTIME_PRINTER));
    assert(reserved && reservations==releases+1);
    assert(ble_runtime_start(BLE_RUNTIME_MUSE,prepare,synchronized)==ESP_ERR_INVALID_STATE);
    stop_hangs=false;fake_current=fake_stop;fake_stop->entry(NULL);fake_current=NULL; // Join the same late stop, not a second one.
    assert(ble_runtime_stop(BLE_RUNTIME_PRINTER)==ESP_OK&&stop_calls==before+1);clean();
    assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_OK);join_delay=2;
    assert(ble_runtime_stop(BLE_RUNTIME_PRINTER)==ESP_OK&&delayed_ticks==2);clean();
    prepare_fails=deinit_fails=true;
    assert(ble_runtime_start(BLE_RUNTIME_MUSE,prepare,synchronized)==ESP_FAIL);
    assert(ble_runtime_owned(BLE_RUNTIME_MUSE)&&runtime_fault&&!host_stack&&!stop_stack);
    assert(reserved && reservations==releases+1);
    assert(ble_runtime_start(BLE_RUNTIME_PRINTER,prepare,synchronized)==ESP_ERR_INVALID_STATE);
    assert(ble_runtime_stop(BLE_RUNTIME_MUSE)==ESP_FAIL);
    puts("BLE runtime actual code: exclusive ownership, 50 release cycles, allocation failures and bounded late shutdown passed");
}
