// Desktop shim: FreeRTOS tasks over detached pthreads, with the one-slot
// task-notification the poll task uses as its wakeup.
#pragma once

#include "freertos/FreeRTOS.h"

typedef struct sim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       uint32_t stack_bytes, void *arg, UBaseType_t prio,
                       TaskHandle_t *out_handle);
void vTaskDelay(TickType_t ticks);

// Notification pair used by state.c (poll wakeup).
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
void xTaskNotifyGive(TaskHandle_t task);
