#include "util.h"
#include "vg.h"
#include <bx/bx.h>

#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
#include <xmmintrin.h>
#include <immintrin.h>
#endif

namespace vg
{
void memset32(void* __restrict dst, uint32_t n, const void* __restrict src)
{
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
	const __m128 s128 = _mm_load_ps1((const float*)src);
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
	case 3: *d++ = *(const float*)src;
	case 2: *d++ = *(const float*)src;
	case 1: *d   = *(const float*)src;
	}
#else
	const uint32_t s = *(const uint32_t*)src;
	uint32_t* d = (uint32_t*)dst;
	while (n-- > 0) {
		*d++ = s;
	}
#endif
}

void memset64(void* __restrict dst, uint32_t n64, const void* __restrict src)
{
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
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
#else
	const uint32_t s0 = *((const uint32_t*)src + 0);
	const uint32_t s1 = *((const uint32_t*)src + 1);
	uint32_t* d = (uint32_t*)dst;
	while (n64-- > 0) {
		d[0] = s0;
		d[1] = s1;
		d += 2;
	}
#endif
}

void memset128(void* __restrict dst, uint32_t n128, const void* __restrict src)
{
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
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
#else
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
#endif
}

void genQuadIndices_unaligned(uint16_t* dst, uint32_t n, uint16_t firstVertexID)
{
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
	BX_ALIGN_DECL(16, static const uint16_t delta[]) = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		8, 9, 10, 8, 10, 11,
		12, 13, 14, 12, 14, 15
	};

	const __m128i xmm_delta0 = _mm_load_si128((const __m128i*)&delta[0]);
	const __m128i xmm_delta1 = _mm_load_si128((const __m128i*)&delta[8]);
	const __m128i xmm_delta2 = _mm_load_si128((const __m128i*)&delta[16]);

	const uint32_t numIter = n >> 2; // 4 quads per iteration
	for (uint32_t i = 0; i < numIter; ++i) {
		const __m128i id = _mm_set1_epi16(firstVertexID);

		const __m128i id0 = _mm_add_epi16(id, xmm_delta0);
		const __m128i id1 = _mm_add_epi16(id, xmm_delta1);
		const __m128i id2 = _mm_add_epi16(id, xmm_delta2);
		_mm_storeu_si128((__m128i*)(dst + 0), id0);
		_mm_storeu_si128((__m128i*)(dst + 8), id1);
		_mm_storeu_si128((__m128i*)(dst + 16), id2);

		dst += 24;
		firstVertexID += 16;
	}

	uint32_t rem = n & 3;
	switch (rem) {
	case 3:
		dst[0] = firstVertexID; dst[1] = firstVertexID + 1; dst[2] = firstVertexID + 2;
		dst[3] = firstVertexID; dst[4] = firstVertexID + 2; dst[5] = firstVertexID + 3;
		dst += 6;
		firstVertexID += 4;
	case 2:
		dst[0] = firstVertexID; dst[1] = firstVertexID + 1; dst[2] = firstVertexID + 2;
		dst[3] = firstVertexID; dst[4] = firstVertexID + 2; dst[5] = firstVertexID + 3;
		dst += 6;
		firstVertexID += 4;
	case 1:
		dst[0] = firstVertexID; dst[1] = firstVertexID + 1; dst[2] = firstVertexID + 2;
		dst[3] = firstVertexID; dst[4] = firstVertexID + 2; dst[5] = firstVertexID + 3;
		dst += 6;
		firstVertexID += 4;
	}
#else
	while (n-- > 0) {
		dst[0] = firstVertexID; dst[1] = firstVertexID + 1; dst[2] = firstVertexID + 2;
		dst[3] = firstVertexID; dst[4] = firstVertexID + 2; dst[5] = firstVertexID + 3;
		dst += 6;
		firstVertexID += 4;
	}
#endif
}
}
