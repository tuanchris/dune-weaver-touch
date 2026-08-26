// Desktop shim: esp_heap_caps.h — one flat heap, caps ignored.
#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_DMA (1 << 2)
#define MALLOC_CAP_8BIT (1 << 3)
#define MALLOC_CAP_DEFAULT (1 << 4)

static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_realloc(void *ptr, size_t size, unsigned caps)
{
    (void)caps;
    return realloc(ptr, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

static inline size_t heap_caps_get_free_size(unsigned caps)
{
    (void)caps;
    return 4 * 1024 * 1024;  // plenty; UI telemetry only
}

static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{
    (void)caps;
    return 2 * 1024 * 1024;
}
