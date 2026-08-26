// Desktop shim: esp_littlefs.h — the "storage" partition becomes a host
// directory (sim/simfs/storage); sim_remap.h rewrites the /storage paths.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    const char *base_path;
    const char *partition_label;
    bool format_if_mount_failed;
    bool dont_mount;
    bool grow_on_mount;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes,
                            size_t *used_bytes);
