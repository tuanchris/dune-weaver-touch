// Desktop shim: FreeRTOS queues (fixed-size item ring + mutex/cond).
#pragma once

#include "freertos/FreeRTOS.h"

#include <stddef.h>

typedef struct sim_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
