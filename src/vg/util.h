#ifndef VG_UTIL_H
#define VG_UTIL_H

#include <stdint.h>

namespace vg
{
void memset32(void* __restrict dst, uint32_t n, const void* __restrict src);
void memset64(void* __restrict dst, uint32_t n64, const void* __restrict src);
void memset128(void* __restrict dst, uint32_t n128, const void* __restrict src);
}

#endif
