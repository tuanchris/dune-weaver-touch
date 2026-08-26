// Single background worker for anything blocking (HTTP, scans, renders).
// LVGL callbacks submit and return; the worker runs jobs FIFO, one at a time.
#include "jobs.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "jobs";

#define JOBS_QUEUE_DEPTH 16
#define JOBS_TASK_STACK 8192  // bytes; task stacks must live in internal RAM
#define JOBS_TASK_PRIO 4

typedef struct {
    job_fn_t fn;
    void *arg;
} job_t;

static QueueHandle_t s_queue;
static QueueHandle_t s_fast_queue;

static void jobs_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    job_t job;
    for (;;) {
        if (xQueueReceive(q, &job, portMAX_DELAY) == pdTRUE) {
            job.fn(job.arg);
        }
    }
}

static esp_err_t spawn_worker(const char *name, QueueHandle_t *q)
{
    *q = xQueueCreate(JOBS_QUEUE_DEPTH, sizeof(job_t));
    ESP_RETURN_ON_FALSE(*q != NULL, ESP_ERR_NO_MEM, TAG, "job queue alloc");

    // xTaskCreate stacks always come from internal RAM (PSRAM is not allowed
    // for task stacks by default), which is what we want here.
    if (xTaskCreate(jobs_task, name, JOBS_TASK_STACK, *q, JOBS_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(*q);
        *q = NULL;
        ESP_LOGE(TAG, "worker task create failed (%s)", name);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t jobs_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;  // idempotent
    }
    ESP_RETURN_ON_ERROR(spawn_worker("jobs", &s_queue), TAG, "slow lane");
    ESP_RETURN_ON_ERROR(spawn_worker("jobs_fast", &s_fast_queue), TAG, "fast lane");
    ESP_LOGI(TAG, "workers up (queue %d, stack %d, prio %d)",
             JOBS_QUEUE_DEPTH, JOBS_TASK_STACK, JOBS_TASK_PRIO);
    return ESP_OK;
}

static esp_err_t submit_to(QueueHandle_t q, job_fn_t fn, void *arg)
{
    ESP_RETURN_ON_FALSE(fn != NULL, ESP_ERR_INVALID_ARG, TAG, "null job fn");
    ESP_RETURN_ON_FALSE(q != NULL, ESP_ERR_INVALID_STATE, TAG, "jobs_init not called");

    const job_t job = { .fn = fn, .arg = arg };
    if (xQueueSend(q, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full — job dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t jobs_submit(job_fn_t fn, void *arg)
{
    return submit_to(s_queue, fn, arg);
}

esp_err_t jobs_submit_fast(job_fn_t fn, void *arg)
{
    return submit_to(s_fast_queue, fn, arg);
}
