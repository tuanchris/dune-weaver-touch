// Desktop shim: FreeRTOS mutex semaphores over pthread mutexes.
#pragma once

#include "freertos/FreeRTOS.h"

typedef struct sim_sem *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
