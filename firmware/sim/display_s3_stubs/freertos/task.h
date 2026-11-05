#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*)(void *),const char *,uint32_t,void *,unsigned,StackType_t *,StaticTask_t *,int);
int xPortGetCoreID(void);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
