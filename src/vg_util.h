#ifndef VG_UTIL_H
#define VG_UTIL_H

#include <stdint.h>

namespace vgutil
{
void memset32(void* __restrict dst, uint32_t n, const void* __restrict src);
void memset64(void* __restrict dst, uint32_t n64, const void* __restrict src);
void memset128(void* __restrict dst, uint32_t n128, const void* __restrict src);

void genQuadIndices_unaligned(uint16_t* dst, uint32_t numQuads, uint16_t firstVertexID);

void batchTransformDrawIndices(const uint16_t* __restrict src, uint32_t n, uint16_t* __restrict dst, uint16_t delta);
void batchTransformPositions(const float* __restrict v, uint32_t n, float* __restrict p, const float* __restrict mtx);

// quads == FONSquad { x1, y1, x2, y2, u1, v1, u2, v2 }
void batchTransformTextQuads(const float* __restrict quads, uint32_t n, const float* __restrict mtx, float* __restrict transformedVertices);

void convertA8_to_RGBA8(uint32_t* rgba, const uint8_t* a8, uint32_t w, uint32_t h, uint32_t rgbColor);

bool invertMatrix3(const float* __restrict t, float* __restrict inv);

inline void transformPos2D(float x, float y, const float* __restrict mtx, float* __restrict res)
{
	res[0] = mtx[0] * x + mtx[2] * y + mtx[4];
	res[1] = mtx[1] * x + mtx[3] * y + mtx[5];
}

inline void transformVec2D(float x, float y, const float* __restrict mtx, float* __restrict res)
{
	res[0] = mtx[0] * x + mtx[2] * y;
	res[1] = mtx[1] * x + mtx[3] * y;
}

inline void multiplyMatrix3(const float* __restrict a, const float* __restrict b, float* __restrict res)
{
	res[0] = a[0] * b[0] + a[2] * b[1];
	res[1] = a[1] * b[0] + a[3] * b[1];
	res[2] = a[0] * b[2] + a[2] * b[3];
	res[3] = a[1] * b[2] + a[3] * b[3];
	res[4] = a[0] * b[4] + a[2] * b[5] + a[4];
	res[5] = a[1] * b[4] + a[3] * b[5] + a[5];
}
}

#endif
