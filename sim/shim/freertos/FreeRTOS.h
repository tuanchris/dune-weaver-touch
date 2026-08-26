// Desktop shim: FreeRTOS types over pthreads. Tick = 1 ms (matches
// CONFIG_FREERTOS_HZ=1000 on the device).
#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdFAIL 0
#define pdPASS 1

#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
