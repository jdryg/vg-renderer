#include "util.h"
#include "common.h"

namespace vg
{
void memset32(void* __restrict dst, uint32_t n, const void* __restrict src)
{
#if !VG_CONFIG_ENABLE_SIMD
	const uint32_t s = *(const uint32_t*)src;
	uint32_t* d = (uint32_t*)dst;
	while (n-- > 0) {
		*d++ = s;
	}
#else
	const __m128 s128 = _mm_load1_ps((const float*)src);
	float* d = (float*)dst;

	uint32_t iter = n >> 4;
	while (iter-- > 0) {
		_mm_storeu_ps(d + 0, s128);
		_mm_storeu_ps(d + 4, s128);
		_mm_storeu_ps(d + 8, s128);
		_mm_storeu_ps(d + 12, s128);
		d += 16;
	}

	uint32_t rem = n & 15;
	if (rem >= 8) {
		_mm_storeu_ps(d + 0, s128);
		_mm_storeu_ps(d + 4, s128);
		d += 8;
		rem -= 8;
	}

	if (rem >= 4) {
		_mm_storeu_ps(d, s128);
		d += 4;
		rem -= 4;
	}

	switch (rem) {
	case 3:
		*d++ = *(const float*)src;
	case 2:
		*d++ = *(const float*)src;
	case 1:
		*d++ = *(const float*)src;
	}
#endif
}

void memset64(void* __restrict dst, uint32_t n64, const void* __restrict src)
{
#if !VG_CONFIG_ENABLE_SIMD
	const uint32_t s0 = *((const uint32_t*)src + 0);
	const uint32_t s1 = *((const uint32_t*)src + 1);
	uint32_t* d = (uint32_t*)dst;
	while (n64-- > 0) {
		d[0] = s0;
		d[1] = s1;
		d += 2;
	}
#else
	const float* s = (const float*)src;
	const __m128 s0 = _mm_load_ss(&s[0]);
	const __m128 s1 = _mm_load_ss(&s[1]);
	const __m128 s0011 = _mm_shuffle_ps(s0, s1, 0);
	const __m128 s128 = _mm_shuffle_ps(s0011, s0011, _MM_SHUFFLE(2, 0, 2, 0));
	float* d = (float*)dst;

	uint32_t iter = n64 >> 3; // 8 64-bit values per iteration (== 16 floats / iter)
	while (iter-- > 0) {
		_mm_storeu_ps(d + 0, s128);
		_mm_storeu_ps(d + 4, s128);
		_mm_storeu_ps(d + 8, s128);
		_mm_storeu_ps(d + 12, s128);
		d += 16;
	}

	uint32_t rem = n64 & 7;
	if (rem >= 4) {
		_mm_storeu_ps(d + 0, s128);
		_mm_storeu_ps(d + 4, s128);
		d += 8;
		rem -= 4;
	}

	if (rem >= 2) {
		_mm_storeu_ps(d, s128);
		d += 4;
		rem -= 2;
	}

	if (rem) {
		d[0] = s[0];
		d[1] = s[1];
	}
#endif
}

void memset128(void* __restrict dst, uint32_t n128, const void* __restrict src)
{
#if !VG_CONFIG_ENABLE_SIMD
	const uint32_t s0 = *((const uint32_t*)src + 0);
	const uint32_t s1 = *((const uint32_t*)src + 1);
	const uint32_t s2 = *((const uint32_t*)src + 2);
	const uint32_t s3 = *((const uint32_t*)src + 3);
	uint32_t* d = (uint32_t*)dst;
	while (n128-- > 0) {
		d[0] = s0;
		d[1] = s1;
		d[2] = s2;
		d[3] = s3;
		d += 4;
	}
#else
	const __m128 s128 = _mm_loadu_ps((const float*)src);
	float* d = (float*)dst;

	uint32_t iter = n128 >> 2; // 4 128-bit values per iteration (== 16 floats / iter)
	while (iter-- > 0) {
		_mm_storeu_ps(d + 0, s128);
		_mm_storeu_ps(d + 4, s128);
		_mm_storeu_ps(d + 8, s128);
		_mm_storeu_ps(d + 12, s128);
		d += 16;
	}

	uint32_t rem = n128 & 3;
	if (rem >= 2) {
		_mm_storeu_ps(d + 0, s128);
		_mm_storeu_ps(d + 4, s128);
		d += 8;
		rem -= 2;
	}

	if (rem) {
		_mm_storeu_ps(d, s128);
	}
#endif
}
}
