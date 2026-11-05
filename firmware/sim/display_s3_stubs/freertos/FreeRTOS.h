#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
#define portENTER_CRITICAL_ISR(lock) portENTER_CRITICAL(lock)
#define portEXIT_CRITICAL_ISR(lock) portEXIT_CRITICAL(lock)
#define portYIELD_FROM_ISR() ((void)0)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configMAX_PRIORITIES 25
#define configUSE_TIME_SLICING 1
typedef struct { pthread_mutex_t lock; pthread_cond_t ready; int count, role; bool initialized; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef struct { pthread_t thread; void (*entry)(void *); void *context; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
