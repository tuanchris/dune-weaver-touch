// Desktop shim: esp_random.h
#pragma once

#include <stdint.h>
#include <stdlib.h>

static inline uint32_t esp_random(void)
{
    return arc4random();
}
