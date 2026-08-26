// Single background worker for anything blocking (HTTP, scans, renders).
// LVGL event callbacks must never block: submit a job, return immediately,
// and update widgets from the job under lvgl_port_lock().
#pragma once

#include "esp_err.h"

typedef void (*job_fn_t)(void *arg);

esp_err_t jobs_init(void);

// Queue a job (FIFO, one at a time). Returns ESP_ERR_NO_MEM if the queue is
// full — surface that instead of silently dropping.
esp_err_t jobs_submit(job_fn_t fn, void *arg);

// Fast lane for short lifeline actions (stop/pause/resume/skip/feed): its own
// worker, so a tap never waits behind a 45 s SD fetch or a 95 s homing job on
// the normal queue. Only submit requests that finish in a few seconds.
esp_err_t jobs_submit_fast(job_fn_t fn, void *arg);
