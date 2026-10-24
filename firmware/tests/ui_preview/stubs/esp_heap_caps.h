/* Host preview stub: every heap is malloc. */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
static inline void *heap_caps_calloc(size_t n, size_t s, unsigned caps) { (void)caps; return calloc(n, s); }
static inline void *heap_caps_malloc(size_t s, unsigned caps) { (void)caps; return malloc(s); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 0; }
