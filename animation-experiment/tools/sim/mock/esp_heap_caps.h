#pragma once
#include <cstdlib>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_8BIT 2
inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
inline size_t heap_caps_get_largest_free_block(int) { return 110000; }
