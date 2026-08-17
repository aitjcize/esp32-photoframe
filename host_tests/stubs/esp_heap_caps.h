// Host-test stub for esp_heap_caps.h — caps are ignored; everything is plain
// malloc so leak checkers still work.
#pragma once

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_DEFAULT 0

static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void) caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void) caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

#ifdef __cplusplus
}
#endif
