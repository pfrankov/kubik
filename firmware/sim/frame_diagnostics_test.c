#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "frame_diagnostics.h"
#include "character.h"
#include "status_json.h"
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define configASSERT assert
#define ESP_LOGI(...) ((void)0)
typedef struct { bool given; } semaphore_t;
typedef semaphore_t *SemaphoreHandle_t;
static semaphore_t semaphores[2];
static int created;
static unsigned clock_tick;
static void (*waiting)(void);
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &semaphores[created++]; }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t result = &semaphores[created++]; result->given = true; return result;
}
static int xSemaphoreGive(SemaphoreHandle_t sem) { sem->given = true; return 1; }
static unsigned xTaskGetTickCount(void) { return clock_tick; }
static int xSemaphoreTake(SemaphoreHandle_t sem, unsigned timeout) {
    if (!sem->given && timeout && waiting) waiting();
    if (!sem->given) { clock_tick += timeout; return 0; }
    sem->given = false; return 1;
}
#define R_FPS 30
typedef int face_mode_t;
#define MODE_BOOT 0
#define MODE_LISTENING 2
/* CADENCE */
/* PRODUCTION */
static void publish(void) {
    frame_diagnostics_snapshot_t sample = {.frames = 66, .elapsed_us = 2200000, .modes = 16, .active = 7, .slow_frames = 3, .deadline_misses = 5};
    frame_diagnostics_publish(&sample);
}
static void old_then_current(void) {
    // A previous request completed after its deadline, and signals this reader.
    s_sequence = 1; s_ready->given = true;
    clock_tick += 10; waiting = publish;
}
int main(void) {
    int64_t average = 0;
    for (int i = 0; i < 8; ++i) frame_rate(7, &average, 40000);
    assert(frame_rate(7, &average, 40000) == 20);
    for (int i = 0; i < 4; ++i) frame_rate(4, &average, 25000);
    assert(frame_rate(4, &average, 25000) == 30); // healthy reply recovers from scatter
    for (int i = 0; i < 8; ++i) assert(frame_rate(4, &average, 31000) == 30);
    for (int i = 0; i < 8; ++i) frame_rate(4, &average, 40000);
    assert(frame_rate(4, &average, 40000) == 20); // actual overload still reduces cadence
    assert(frame_rate(MODE_LISTENING, &average, 25000) == 30);
    assert(frame_rate(MODE_LISTENING, &average, 40000) == 30);
    assert(frame_rate(MODE_LISTENING, &average, 40000) == 20); // listening retains real overload protection
    assert(frame_rate(4, &average, 27000) == 30); // transition owns its workload, not old listening load
    for(int i=0;i<12;i++) {
        assert(frame_rate(4,&average,32000)==30);
        assert(frame_rate(4,&average,36000)==30); // isolated allowed transient never starts 20 fps
        assert(frame_rate(4,&average,27000)==30);
    }
    assert(frame_rate(4,&average,40000)==30);
    assert(frame_rate(4,&average,40000)==20); // two consecutive overloaded frames retain protection
    for(int i=0;i<8;i++)frame_rate(4,&average,31500);
    assert(frame_rate(4,&average,31500)==30); // healthy reaction recovers inside its real slot
    char reply[2000];
    frame_diagnostics_init();
    frame_diagnostics_reply(reply, sizeof reply);
    assert(strstr(reply, "\"ok\":false"));
    assert(!frame_diagnostics_requested());
    waiting = old_then_current;
    frame_diagnostics_reply(reply, sizeof reply);
    assert(strstr(reply, "\"sequence\":2"));
    assert(strstr(reply, "\"modes\":16"));
    assert(strstr(reply, "\"frames\":66"));
    assert(strstr(reply, "\"slow_frames\":3"));
    assert(strstr(reply, "\"deadline_misses\":5"));
    waiting = publish;
    frame_diagnostics_reply(reply, 32);
    assert(!strcmp(reply, "{\"ok\":false}"));
    waiting = NULL;
    frame_diagnostics_reply(reply, sizeof reply);
    assert(strstr(reply, "\"ok\":false")); // old successful data is never reused
    s_reader->given = false;
    frame_diagnostics_reply(reply, sizeof reply);
    assert(strstr(reply, "busy"));
    puts("frame diagnostics: matching request, delayed stale notification, timeout, truncation and busy reader passed");
}
