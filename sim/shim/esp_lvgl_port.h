// Desktop shim: esp_lvgl_port.h — the recursive LVGL lock only. The real
// component also owns the LVGL task; in the sim the main loop plays that role.
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
