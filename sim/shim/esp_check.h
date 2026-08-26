// Desktop shim: esp_check.h — return/goto helper macros.
#pragma once

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...)                                 \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                              \
            ESP_LOGE(tag, "%s(%d): " fmt, __func__, __LINE__, ##__VA_ARGS__); \
            return err_rc_;                                                   \
        }                                                                     \
    } while (0)

#define ESP_RETURN_ON_FALSE(cond, err_code, tag, fmt, ...)                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ESP_LOGE(tag, "%s(%d): " fmt, __func__, __LINE__, ##__VA_ARGS__); \
            return err_code;                                                  \
        }                                                                     \
    } while (0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, fmt, ...)                         \
    do {                                                                      \
        ret = (x);                                                            \
        if (ret != ESP_OK) {                                                  \
            ESP_LOGE(tag, "%s(%d): " fmt, __func__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                    \
        }                                                                     \
    } while (0)

#define ESP_GOTO_ON_FALSE(cond, err_code, goto_tag, tag, fmt, ...)            \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ret = err_code;                                                   \
            ESP_LOGE(tag, "%s(%d): " fmt, __func__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                    \
        }                                                                     \
    } while (0)
