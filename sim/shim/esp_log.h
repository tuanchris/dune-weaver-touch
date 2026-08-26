// Desktop shim: esp_log.h — leveled logging to stderr, ESP-style prefixes.
#pragma once

#include <stdio.h>
#include <stdint.h>

int64_t sim_log_ms(void);

#define SIM_LOG(letter, tag, fmt, ...)                                        \
    fprintf(stderr, "%s (%lld) %s: " fmt "\n", letter,                        \
            (long long)sim_log_ms(), tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) SIM_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) SIM_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) SIM_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) SIM_LOG("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) SIM_LOG("V", tag, fmt, ##__VA_ARGS__)
