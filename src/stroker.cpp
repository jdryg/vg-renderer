#include <vg/stroker.h>
#include "vg_util.h"
#include "libtess2/tesselator.h"
#include <bx/allocator.h>
#include <bx/math.h>
#include <string.h> // memcpy

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4127) // conditional expression is constant
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4456) // declaration of X hides previous local decleration
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wshadow")

#define RSQRT_ALGORITHM 1
#define RCP_ALGORITHM 1

namespace vg
{
struct Vec2
{
	float x, y;
};

inline Vec2 vec2Add(const Vec2& a, const Vec2& b)    { return{ a.x + b.x, a.y + b.y }; }
inline Vec2 vec2Sub(const Vec2& a, const Vec2& b)    { return{ a.x - b.x, a.y - b.y }; }
inline Vec2 vec2Scale(const Vec2& a, float s)        { return{ a.x * s, a.y * s }; }
inline Vec2 vec2PerpCCW(const Vec2& a)               { return{ -a.y, a.x }; }
inline Vec2 vec2PerpCW(const Vec2& a)                { return{ a.y, -a.x }; }
inline float vec2Cross(const Vec2& a, const Vec2& b) { return a.x * b.y - b.x * a.y; }
inline float vec2Dot(const Vec2& a, const Vec2& b)   { return a.x * b.x + a.y * b.y; }

// Direction from a to b
inline Vec2 vec2Dir(const Vec2& a, const Vec2& b)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float lenSqr = dx * dx + dy * dy;
	const float invLen = lenSqr < VG_EPSILON ? 0.0f : bx::rsqrt(lenSqr);
	return{ dx * invLen, dy * invLen };
}

inline Vec2 calcExtrusionVector(const Vec2& d01, const Vec2& d12)
{
	// v is the vector from the path point to the outline point, assuming a stroke width of 1.0.
	// Equation obtained by solving the intersection of the 2 line segments. d01 and d12 are 
	// assumed to be normalized.
	static const float kMaxExtrusionScale = 1.0f / 100.0f;
	Vec2 v = vec2PerpCCW(d01);
	const float cross = vec2Cross(d12, d01);
	if (bx::abs(cross) > kMaxExtrusionScale) {
		v = vec2Scale(vec2Sub(d01, d12), (1.0f / cross));
	}

	return v;
}

#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
static const __m128 vec2_perpCCW_xorMask = _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, 0x80000000));

static inline __m128 xmm_vec2_rotCCW90(const __m128 a)
{
	__m128 ayx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 2, 0, 1)); // { a.y, a.x, DC, DC }
	return _mm_xor_ps(ayx, vec2_perpCCW_xorMask); // { -a.y, a.x, DC, DC }
}

static inline float xmm_vec2_cross(const __m128 a, const __m128 b)
{
	const __m128 axy_bxy = _mm_movelh_ps(a, b); // { a.x, a.y, b.x, b.y }
	const __m128 byx_ayx = _mm_shuffle_ps(axy_bxy, axy_bxy, _MM_SHUFFLE(0, 1, 2, 3)); // { b.y, b.x, a.y, a.x }
	const __m128 axby_aybx = _mm_mul_ps(axy_bxy, byx_ayx); // { a.x * b.y, a.y * b.x, b.x * a.y, b.y * a.x }
	const __m128 bxay = _mm_shuffle_ps(axby_aybx, axby_aybx, _MM_SHUFFLE(1, 1, 1, 1)); // { a.y * b.x, a.y * b.x, a.y * b.x, a.y * b.x }
	const __m128 cross = _mm_sub_ss(axby_aybx, bxay);
	return _mm_cvtss_f32(cross);
}

static inline __m128 xmm_vec2_dir(const __m128 a, const __m128 b)
{
	const __m128 dxy = _mm_sub_ps(b, a); // { dx, dy, DC, DC }
	const __m128 dxySqr = _mm_mul_ps(dxy, dxy); // { dx * dx, dy * dy, DC, DC }
	const __m128 dySqr = _mm_shuffle_ps(dxySqr, dxySqr, _MM_SHUFFLE(1, 1, 1, 1)); // { dy * dy, dy * dy, dy * dy, dy * dy }
	const float lenSqr = _mm_cvtss_f32(_mm_add_ss(dxySqr, dySqr));
	__m128 dir = _mm_setzero_ps();
	if (lenSqr >= VG_EPSILON) {
		const __m128 invLen = _mm_set_ps1(bx::rsqrt(lenSqr));
		dir = _mm_mul_ps(dxy, invLen);
	}
	return dir;
}

static inline __m128 xmm_calcExtrusionVector(const __m128 d01, const __m128 d12)
{
	const float cross = xmm_vec2_cross(d12, d01);
#if 0
	return (bx::abs(cross) > VG_EPSILON) ? _mm_mul_ps(_mm_sub_ps(d01, d12), _mm_set_ps1(rcp(cross))) : xmm_vec2_rotCCW90(d01);
#else
	return (bx::abs(cross) > VG_EPSILON) ? _mm_mul_ps(_mm_sub_ps(d01, d12), _mm_set_ps1(1.0f / cross)) : xmm_vec2_rotCCW90(d01);
#endif
}

static inline __m128 xmm_rsqrt(__m128 a)
{
#if RSQRT_ALGORITHM == 0
	const __m128 res = _mm_div_ps(xmm_one, _mm_sqrt_ps(a));
#elif RSQRT_ALGORITHM == 1
	const __m128 res = _mm_rsqrt_ps(a);
#elif RSQRT_ALGORITHM == 2
	// Newton/Raphson
	const __m128 rsqrtEst = _mm_rsqrt_ps(a);
	const __m128 iter0 = _mm_mul_ps(a, rsqrtEst);
	const __m128 iter1 = _mm_mul_ps(iter0, rsqrtEst);
	const __m128 half_rsqrt = _mm_mul_ps(xmm_half, rsqrtEst);
	const __m128 three_sub_iter1 = _mm_sub_ps(xmm_three, iter1);
	const __m128 res = _mm_mul_ps(half_rsqrt, three_sub_iter1);
#endif

	return res;
}

static inline __m128 xmm_rcp(__m128 a)
{
#if RCP_ALGORITHM == 0
	const __m128 inv_a = _mm_div_ps(xmm_one, a);
#elif RCP_ALGORITHM == 1
	const __m128 inv_a = _mm_rcp_ps(a);
#elif RCP_ALGORITHM == 2
	// TODO: 
#endif

	return inv_a;
}
#endif

struct libtess2Allocator
{
	uint8_t* m_Buffer;
	uint32_t m_Capacity;
	uint32_t m_Size;
};

static void* libtess2Alloc(void* userData, uint32_t size)
{
	libtess2Allocator* alloc = (libtess2Allocator*)userData;
	if (alloc->m_Size + size > alloc->m_Capacity) {
		return nullptr;
	}

	// Align all allocations to 16 bytes
	uint32_t offset = (alloc->m_Size & ~0x0F) + ((alloc->m_Size & 0x0F) != 0 ? 0x10 : 0);

	uint8_t* mem = &alloc->m_Buffer[offset];
	alloc->m_Size = offset + size;
	return mem;
}

static void libtess2Free(void* userData, void* ptr)
{
	// Don't do anything!
	BX_UNUSED(userData, ptr);
}

struct Stroker
{
	bx::AllocatorI* m_Allocator;
	Vec2* m_PosBuffer;
	uint32_t* m_ColorBuffer;
	uint16_t* m_IndexBuffer;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
	uint32_t m_VertexCapacity;
	uint32_t m_IndexCapacity;
	TESStesselator* m_Tesselator;
	libtess2Allocator m_libTessAllocator;
	float m_FringeWidth;
	float m_Scale;
	float m_TesselationTolerance;
};

static void resetGeometry(Stroker* stroker);
static void expandIB(Stroker* stroker, uint32_t n);
static void expandVB(Stroker* stroker, uint32_t n);

template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
static void polylineStroke(Stroker* stroker, Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth);
template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
static void polylineStrokeAA(Stroker* stroker, Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth, Color color);
template<LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
static void polylineStrokeAAThin(Stroker* stroker, Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, Color color, bool closed);

template<uint32_t N>
static void addPos(Stroker* stroker, const Vec2* srcPos);
template<uint32_t N>
static void addPosColor(Stroker* stroker, const Vec2* srcPos, const uint32_t* srcColor);
template<uint32_t N>
static void addIndices(Stroker* stroker, const uint16_t* src);

Stroker* createStroker(bx::AllocatorI* allocator)
{
	Stroker* stroker = (Stroker*)BX_ALLOC(allocator, sizeof(Stroker));
	bx::memSet(stroker, 0, sizeof(Stroker));
	stroker->m_Allocator = allocator;
	stroker->m_FringeWidth = 1.0f;
	stroker->m_Scale = 1.0f;
	stroker->m_TesselationTolerance = 0.25f;
	return stroker;
}

void destroyStroker(Stroker* stroker)
{
	bx::AllocatorI* allocator = stroker->m_Allocator;

	BX_ALIGNED_FREE(allocator, stroker->m_PosBuffer, 16);
	BX_ALIGNED_FREE(allocator, stroker->m_ColorBuffer, 16);
	BX_ALIGNED_FREE(allocator, stroker->m_IndexBuffer, 16);

	if (stroker->m_Tesselator) {
		tessDeleteTess(stroker->m_Tesselator);
	}

	BX_ALIGNED_FREE(allocator, stroker->m_libTessAllocator.m_Buffer, 16);

	BX_FREE(allocator, stroker);
}

void strokerReset(Stroker* stroker, float scale, float tesselationTolerance, float fringeWidth)
{
	stroker->m_Scale = scale;
	stroker->m_TesselationTolerance = tesselationTolerance;
	stroker->m_FringeWidth = fringeWidth;
}

void strokerPolylineStroke(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numPathVertices, bool isClosed, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	const uint8_t perm = (((uint8_t)lineCap) << 1)
		| (((uint8_t)lineJoin) << 3)
		| (isClosed ? 0x01 : 0x00);

	const Vec2* vtx = (const Vec2*)vertexList;

	switch (perm) {
	case  0: polylineStroke<false, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth);   break;
	case  1: polylineStroke<true, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
	case  2: polylineStroke<false, LineCap::Round, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth);  break;
	case  3: polylineStroke<true, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
	case  4: polylineStroke<false, LineCap::Square, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth); break;
	case  5: polylineStroke<true, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
		// 6 to 7 == invalid line cap type
	case  8: polylineStroke<false, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth);   break;
	case  9: polylineStroke<true, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
	case 10: polylineStroke<false, LineCap::Round, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth);  break;
	case 11: polylineStroke<true, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
	case 12: polylineStroke<false, LineCap::Square, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth); break;
	case 13: polylineStroke<true, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
		// 14 to 15 == invalid line cap type
	case 16: polylineStroke<false, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth);   break;
	case 17: polylineStroke<true, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
	case 18: polylineStroke<false, LineCap::Round, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth);  break;
	case 19: polylineStroke<true, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
	case 20: polylineStroke<false, LineCap::Square, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth); break;
	case 21: polylineStroke<true, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth);    break;
		// 22 to 32 == invalid line join type
	default:
		VG_WARN(false, "Invalid stroke configuration");
		break;
	}
}

void strokerPolylineStrokeAA(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numPathVertices, bool isClosed, Color color, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	const uint8_t perm = (((uint8_t)lineCap) << 1)
		| (((uint8_t)lineJoin) << 3)
		| (isClosed ? 0x01 : 0x00);

	const Vec2* vtx = (const Vec2*)vertexList;

	switch (perm) {
	case  0: polylineStrokeAA<false, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);   break;
	case  1: polylineStrokeAA<true, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case  2: polylineStrokeAA<false, LineCap::Round, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);  break;
	case  3: polylineStrokeAA<true, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case  4: polylineStrokeAA<false, LineCap::Square, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth, color); break;
	case  5: polylineStrokeAA<true, LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
		// 6 to 7 == invalid line cap type
	case  8: polylineStrokeAA<false, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);   break;
	case  9: polylineStrokeAA<true, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 10: polylineStrokeAA<false, LineCap::Round, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);  break;
	case 11: polylineStrokeAA<true, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 12: polylineStrokeAA<false, LineCap::Square, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth, color); break;
	case 13: polylineStrokeAA<true, LineCap::Butt, LineJoin::Round>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
		// 14 to 15 == invalid line cap type
	case 16: polylineStrokeAA<false, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);   break;
	case 17: polylineStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 18: polylineStrokeAA<false, LineCap::Round, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);  break;
	case 19: polylineStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 20: polylineStrokeAA<false, LineCap::Square, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth, color); break;
	case 21: polylineStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, strokeWidth, color);    break;
		// 22 to 32 == invalid line join type
	default:
		VG_WARN(false, "Invalid stroke configuration");
		break;
	}
}

void strokerPolylineStrokeAAThin(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numPathVertices, bool isClosed, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	// TODO: Why is isClosed passed as argument instead of template param?
	const uint8_t perm = ((uint8_t)lineCap) | (((uint8_t)lineJoin) << 2);

	const Vec2* vtx = (const Vec2*)vertexList;

	switch (perm) {
	case  0: polylineStrokeAAThin<LineCap::Butt, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  1: polylineStrokeAAThin<LineCap::Square, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  2: polylineStrokeAAThin<LineCap::Square, LineJoin::Miter>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  4: polylineStrokeAAThin<LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  5: polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  6: polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  8: polylineStrokeAAThin<LineCap::Butt, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case  9: polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	case 10: polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(stroker, mesh, vtx, numPathVertices, color, isClosed);   break;
	default:
		VG_WARN(false, "Invalid stroke configuration");
		break;
	}
}

void strokerConvexFill(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices)
{
	const uint32_t numTris = numVertices - 2;
	const uint32_t numIndices = numTris * 3; // N - 2 triangles in a N pt fan, 3 indices per triangle

	resetGeometry(stroker);

	// Index Buffer
	{
		expandIB(stroker, numIndices);

		uint16_t* dstIndex = stroker->m_IndexBuffer;
		uint16_t nextID = 1;

		uint32_t n = numTris;
		while (n-- > 0) {
			*dstIndex++ = 0;
			*dstIndex++ = nextID;
			*dstIndex++ = nextID + 1;

			++nextID;
		}

		stroker->m_NumIndices += numIndices;
	}

	mesh->m_PosBuffer = vertexList;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = numVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}

#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
void strokerConvexFillAA(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, uint32_t color)
{
	VG_CHECK(numVertices >= 3, "Invalid number of vertices");

	const uint32_t lastVertexID = numVertices - 1;

	const __m128 vtx0 = _mm_loadl_pi(_mm_setzero_ps(), (const __m64*)vertexList);
	const __m128 vtx1 = _mm_loadl_pi(_mm_setzero_ps(), (const __m64*)(vertexList + 2));
	const __m128 vtx2 = _mm_loadl_pi(_mm_setzero_ps(), (const __m64*)(vertexList + 4));
	const float cross = xmm_vec2_cross(_mm_sub_ps(vtx1, vtx0), _mm_sub_ps(vtx2, vtx0));

	const float aa = stroker->m_FringeWidth * 0.5f * bx::sign(cross);
	const __m128 xmm_aa = _mm_set_ps1(aa);

	const uint32_t c0 = colorSetAlpha(color, 0);

	const uint32_t numTris =
		(numVertices - 2) + // Triangle fan
		(numVertices * 2); // AA fringes
	const uint32_t numDrawVertices = numVertices * 2; // original polygon point + AA fringe point.
	const uint32_t numDrawIndices = numTris * 3;

	resetGeometry(stroker);

	// Vertex buffer
	{
		expandVB(stroker, numDrawVertices);

		const __m128 vtxLast = _mm_loadl_pi(_mm_setzero_ps(), (const __m64*)(vertexList + (lastVertexID << 1)));
		__m128 d01 = xmm_vec2_dir(vtxLast, vtx0);
		__m128 p1 = vtx0;

		const float* srcPos = vertexList + 2;
		float* dstPos = &stroker->m_PosBuffer->x;

		const __m128 xmm_epsilon = _mm_set_ps1(VG_EPSILON);
		const __m128 vec2x2_perpCCW_xorMask = _mm_castsi128_ps(_mm_set_epi32(0, 0x80000000, 0, 0x80000000));

		const uint32_t numIter = lastVertexID >> 2;
		for (uint32_t i = 0; i < numIter; ++i) {
			// Load 4 points. With p1 from previous loop iteration make up 4 segments
			const __m128 p23 = _mm_loadu_ps(srcPos);                          // { p2.x, p2.y, p3.x, p3.y }
			const __m128 p45 = _mm_loadu_ps(srcPos + 4);                      // { p4.x, p4.y, p5.x, p5.y }

			const __m128 p12 = _mm_movelh_ps(p1, p23);                        // { p1.x, p1.y, p2.x, p2.y }
			const __m128 p34 = _mm_movelh_ps(_mm_movehl_ps(p23, p23), p45);   // { p3.x, p3.y, p4.x, p4.y }

			// Calculate the direction vector of the 4 segments
			// NOTE: Tried to calc all 4 rsqrt in 1 call but it ends up being slower. Kept this version for now.
			const __m128 d12_23_unorm = _mm_sub_ps(p23, p12);
			const __m128 d34_45_unorm = _mm_sub_ps(p45, p34);

			const __m128 d12_23_xy_sqr = _mm_mul_ps(d12_23_unorm, d12_23_unorm);
			const __m128 d34_45_xy_sqr = _mm_mul_ps(d34_45_unorm, d34_45_unorm);

			const __m128 d12_23_yx_sqr = _mm_shuffle_ps(d12_23_xy_sqr, d12_23_xy_sqr, _MM_SHUFFLE(2, 3, 0, 1));
			const __m128 d34_45_yx_sqr = _mm_shuffle_ps(d34_45_xy_sqr, d34_45_xy_sqr, _MM_SHUFFLE(2, 3, 0, 1));

			const __m128 len12_23_sqr = _mm_add_ps(d12_23_xy_sqr, d12_23_yx_sqr);
			const __m128 len34_45_sqr = _mm_add_ps(d34_45_xy_sqr, d34_45_yx_sqr);

			const __m128 lenSqr123_ge_eps = _mm_cmpge_ps(len12_23_sqr, xmm_epsilon);
			const __m128 lenSqr345_ge_eps = _mm_cmpge_ps(len34_45_sqr, xmm_epsilon);

			const __m128 invLen12_23 = xmm_rsqrt(len12_23_sqr);
			const __m128 invLen34_45 = xmm_rsqrt(len34_45_sqr);

			const __m128 invLen12_23_masked = _mm_and_ps(invLen12_23, lenSqr123_ge_eps);
			const __m128 invLen34_45_masked = _mm_and_ps(invLen34_45, lenSqr345_ge_eps);

			const __m128 d12_23 = _mm_mul_ps(d12_23_unorm, invLen12_23_masked);
			const __m128 d34_45 = _mm_mul_ps(d34_45_unorm, invLen34_45_masked);

			// Calculate the 4 extrusion vectors for the 4 points based on the equ
			// abs(cross(d12, d01) > epsilon ? ((d01 - d12) / cross(d12, d01)) : rot90CCW(d01)
			const __m128 v012_123_fake = _mm_xor_ps(_mm_shuffle_ps(d01, d12_23, _MM_SHUFFLE(0, 1, 0, 1)), vec2x2_perpCCW_xorMask);
			const __m128 v234_345_fake = _mm_xor_ps(_mm_shuffle_ps(d12_23, d34_45, _MM_SHUFFLE(0, 1, 2, 3)), vec2x2_perpCCW_xorMask);

			// cross012 = d12.x * d01.y - d12.y * d01.x
			// cross123 = d23.x * d12.y - d23.y * d12.x
			// cross234 = d34.x * d23.y - d34.y * d23.x
			// cross345 = d45.x * d34.y - d45.y * d34.x
			const __m128 dxy01_12 = _mm_shuffle_ps(d01, d12_23, _MM_SHUFFLE(1, 0, 1, 0));
			const __m128 dxy12_23 = d12_23;
			const __m128 dxy23_34 = _mm_shuffle_ps(d12_23, d34_45, _MM_SHUFFLE(1, 0, 3, 2));
			const __m128 dxy34_45 = d34_45;

			const __m128 dx01_12_23_34 = _mm_shuffle_ps(dxy01_12, dxy23_34, _MM_SHUFFLE(2, 0, 2, 0));
			const __m128 dy01_12_23_34 = _mm_shuffle_ps(dxy01_12, dxy23_34, _MM_SHUFFLE(3, 1, 3, 1));
			const __m128 dx12_23_34_45 = _mm_shuffle_ps(dxy12_23, dxy34_45, _MM_SHUFFLE(2, 0, 2, 0));
			const __m128 dy12_23_34_45 = _mm_shuffle_ps(dxy12_23, dxy34_45, _MM_SHUFFLE(3, 1, 3, 1));

			const __m128 crossx012_123_234_345 = _mm_mul_ps(dx12_23_34_45, dy01_12_23_34);
			const __m128 crossy012_123_234_345 = _mm_mul_ps(dy12_23_34_45, dx01_12_23_34);

			const __m128 cross012_123_234_345 = _mm_sub_ps(crossx012_123_234_345, crossy012_123_234_345);

			const __m128 inv_cross012_123_234_345 = xmm_rcp(cross012_123_234_345);

			const __m128 cross_gt_eps012_123_234_345 = _mm_cmpge_ps(cross012_123_234_345, xmm_epsilon);

			const __m128 inv_cross012_123 = _mm_shuffle_ps(inv_cross012_123_234_345, inv_cross012_123_234_345, _MM_SHUFFLE(1, 1, 0, 0));
			const __m128 inv_cross234_345 = _mm_shuffle_ps(inv_cross012_123_234_345, inv_cross012_123_234_345, _MM_SHUFFLE(3, 3, 2, 2));

			const __m128 cross012_123_gt_eps = _mm_shuffle_ps(cross_gt_eps012_123_234_345, cross_gt_eps012_123_234_345, _MM_SHUFFLE(1, 1, 0, 0));
			const __m128 cross234_345_gt_eps = _mm_shuffle_ps(cross_gt_eps012_123_234_345, cross_gt_eps012_123_234_345, _MM_SHUFFLE(3, 3, 2, 2));

			const __m128 dxy012_123 = _mm_sub_ps(dxy01_12, dxy12_23);
			const __m128 dxy234_345 = _mm_sub_ps(dxy23_34, dxy34_45);

			const __m128 v012_123_true = _mm_mul_ps(dxy012_123, inv_cross012_123);
			const __m128 v234_345_true = _mm_mul_ps(dxy234_345, inv_cross234_345);

			const __m128 v012_123_true_masked = _mm_and_ps(cross012_123_gt_eps, v012_123_true);
			const __m128 v234_345_true_masked = _mm_and_ps(cross234_345_gt_eps, v234_345_true);

			const __m128 v012_123_fake_masked = _mm_andnot_ps(cross012_123_gt_eps, v012_123_fake);
			const __m128 v245_345_fake_masked = _mm_andnot_ps(cross234_345_gt_eps, v234_345_fake);

			const __m128 v012_123 = _mm_or_ps(v012_123_true_masked, v012_123_fake_masked);
			const __m128 v234_345 = _mm_or_ps(v234_345_true_masked, v245_345_fake_masked);

			const __m128 v012_v123_aa = _mm_mul_ps(v012_123, xmm_aa);
			const __m128 v234_v345_aa = _mm_mul_ps(v234_345, xmm_aa);

			// Calculate the 2 fringe points for each of p1, p2, p3 and p4
			const __m128 posEdge12 = _mm_add_ps(p12, v012_v123_aa);
			const __m128 negEdge12 = _mm_sub_ps(p12, v012_v123_aa);
			const __m128 posEdge34 = _mm_add_ps(p34, v234_v345_aa);
			const __m128 negEdge34 = _mm_sub_ps(p34, v234_v345_aa);

			const __m128 p1_in_out = _mm_shuffle_ps(posEdge12, negEdge12, _MM_SHUFFLE(1, 0, 1, 0));
			const __m128 p2_in_out = _mm_shuffle_ps(posEdge12, negEdge12, _MM_SHUFFLE(3, 2, 3, 2));
			const __m128 p3_in_out = _mm_shuffle_ps(posEdge34, negEdge34, _MM_SHUFFLE(1, 0, 1, 0));
			const __m128 p4_in_out = _mm_shuffle_ps(posEdge34, negEdge34, _MM_SHUFFLE(3, 2, 3, 2));

			// Store the fringe points
			_mm_store_ps(dstPos + 0, p1_in_out);
			_mm_store_ps(dstPos + 4, p2_in_out);
			_mm_store_ps(dstPos + 8, p3_in_out);
			_mm_store_ps(dstPos + 12, p4_in_out);

			// Move on to the next iteration.
			d01 = _mm_movehl_ps(d34_45, d34_45);
			p1 = _mm_movehl_ps(p45, p45); // p1 = p5
			srcPos += 8;
			dstPos += 16;
		}

		uint32_t rem = (lastVertexID & 3);
		if (rem >= 2) {
			const __m128 p23 = _mm_loadu_ps(srcPos);
			const __m128 p12 = _mm_movelh_ps(p1, p23);

			const __m128 d12_23 = _mm_sub_ps(p23, p12);
			const __m128 d12_23_xy_sqr = _mm_mul_ps(d12_23, d12_23);
			const __m128 d12_23_yx_sqr = _mm_shuffle_ps(d12_23_xy_sqr, d12_23_xy_sqr, _MM_SHUFFLE(2, 3, 0, 1));
			const __m128 len12_23_sqr = _mm_add_ps(d12_23_xy_sqr, d12_23_yx_sqr);
			const __m128 lenSqr_ge_eps = _mm_cmpge_ps(len12_23_sqr, xmm_epsilon);

			const __m128 invLen12_23 = xmm_rsqrt(len12_23_sqr);

			const __m128 invLen12_23_masked = _mm_and_ps(invLen12_23, lenSqr_ge_eps);
			const __m128 d12_23_norm = _mm_mul_ps(d12_23, invLen12_23_masked);

			const __m128 d12 = _mm_movelh_ps(d12_23_norm, d12_23_norm);
			const __m128 d23 = _mm_movehl_ps(d12_23_norm, d12_23_norm);

			const __m128 d12xy_d01xy = _mm_movelh_ps(d12, d01);
			const __m128 d23xy_d12xy = _mm_movelh_ps(d23, d12);

			const __m128 d01yx_d12yx = _mm_shuffle_ps(d12xy_d01xy, d12xy_d01xy, _MM_SHUFFLE(0, 1, 2, 3));
			const __m128 d12yx_d23yx = _mm_shuffle_ps(d23xy_d12xy, d23xy_d12xy, _MM_SHUFFLE(0, 1, 2, 3));

			const __m128 d12xd01y_d12yd01x = _mm_mul_ps(d12xy_d01xy, d01yx_d12yx);
			const __m128 d23xd12y_d23yd12x = _mm_mul_ps(d23xy_d12xy, d12yx_d23yx);

			const __m128 d12yd01x_d23yd12x = _mm_shuffle_ps(d12xd01y_d12yd01x, d23xd12y_d23yd12x, _MM_SHUFFLE(1, 1, 1, 1));
			const __m128 d12xd01y_d23xd12x = _mm_shuffle_ps(d12xd01y_d12yd01x, d23xd12y_d23yd12x, _MM_SHUFFLE(0, 0, 0, 0));

			const __m128 cross012_123 = _mm_sub_ps(d12xd01y_d23xd12x, d12yd01x_d23yd12x);

			const __m128 inv_cross012_123 = xmm_rcp(cross012_123);

			const __m128 v012_123_fake = _mm_xor_ps(d01yx_d12yx, vec2x2_perpCCW_xorMask);

			const __m128 d01xy_d12xy = _mm_shuffle_ps(d12xy_d01xy, d12xy_d01xy, _MM_SHUFFLE(1, 0, 3, 2));
			const __m128 d12xy_d23xy = _mm_shuffle_ps(d23xy_d12xy, d23xy_d12xy, _MM_SHUFFLE(1, 0, 3, 2));

			const __m128 d012xy_d123xy = _mm_sub_ps(d01xy_d12xy, d12xy_d23xy);
			const __m128 v012_123_true = _mm_mul_ps(d012xy_d123xy, inv_cross012_123);

			const __m128 cross_gt_eps = _mm_cmpge_ps(cross012_123, xmm_epsilon);
			const __m128 v012_123_true_masked = _mm_and_ps(cross_gt_eps, v012_123_true);
			const __m128 v012_123_fake_masked = _mm_andnot_ps(cross_gt_eps, v012_123_fake);
			const __m128 v012_123 = _mm_or_ps(v012_123_true_masked, v012_123_fake_masked);

			const __m128 v012_v123_aa = _mm_mul_ps(v012_123, xmm_aa);

			const __m128 posEdge = _mm_add_ps(p12, v012_v123_aa);
			const __m128 negEdge = _mm_sub_ps(p12, v012_v123_aa);

			const __m128 packed0 = _mm_shuffle_ps(posEdge, negEdge, _MM_SHUFFLE(1, 0, 1, 0));
			const __m128 packed1 = _mm_shuffle_ps(posEdge, negEdge, _MM_SHUFFLE(3, 2, 3, 2));

			_mm_store_ps(dstPos, packed0);
			_mm_store_ps(dstPos + 4, packed1);

			dstPos += 8;
			srcPos += 4;
			d01 = d23;
			p1 = _mm_movehl_ps(p23, p23);

			rem -= 2;
		}

		if (rem) {
			const __m128 p2 = _mm_loadl_pi(_mm_setzero_ps(), (const __m64*)srcPos);
			const __m128 d12 = xmm_vec2_dir(p1, p2);
			const __m128 v_aa = _mm_mul_ps(xmm_calcExtrusionVector(d01, d12), xmm_aa);
			const __m128 packed = _mm_movelh_ps(_mm_add_ps(p1, v_aa), _mm_sub_ps(p1, v_aa));
			_mm_store_ps(dstPos, packed);

			dstPos += 4;
			srcPos += 2;
			d01 = d12;
			p1 = p2;
		}

		// Last segment
		{
			const __m128 v_aa = _mm_mul_ps(xmm_calcExtrusionVector(d01, xmm_vec2_dir(p1, vtx0)), xmm_aa);
			const __m128 packed = _mm_movelh_ps(_mm_add_ps(p1, v_aa), _mm_sub_ps(p1, v_aa));
			_mm_store_ps(dstPos, packed);
		}

		const uint32_t colors[2] = { color, c0 };
		vgutil::memset64(stroker->m_ColorBuffer, numVertices, &colors[0]);

		stroker->m_NumVertices += numDrawVertices;
	}

	// Index buffer
	{
		expandIB(stroker, numDrawIndices);

		uint16_t* dstIndex = stroker->m_IndexBuffer;

		// First fringe quad
		dstIndex[0] = 0; dstIndex[1] = 1; dstIndex[2] = 3;
		dstIndex[3] = 0; dstIndex[4] = 3; dstIndex[5] = 2;
		dstIndex += 6;

		const uint32_t numFanTris = numVertices - 2;

		__m128i xmm_stv = _mm_set1_epi16(2);
		{
			static const uint16_t delta0[8] = { 0, 0, 2, 0, 1, 3, 0, 3 };
			static const uint16_t delta1[8] = { 2, 0, 2, 4, 2, 3, 5, 2 };
			static const uint16_t delta2[8] = { 5, 4, 0, 4, 6, 4, 5, 7 };
			static const uint16_t delta3[8] = { 4, 7, 6, 0, 6, 8, 6, 7 };
			static const uint16_t delta4[8] = { 9, 6, 9, 8, 0, 0, 0, 0 };
			const __m128i xmm_delta0 = _mm_loadu_si128((const __m128i*)delta0);
			const __m128i xmm_delta1 = _mm_loadu_si128((const __m128i*)delta1);
			const __m128i xmm_delta2 = _mm_loadu_si128((const __m128i*)delta2);
			const __m128i xmm_delta3 = _mm_loadu_si128((const __m128i*)delta3);
			const __m128i xmm_delta4 = _mm_loadu_si128((const __m128i*)delta4);

			const __m128i xmm_stv_delta = _mm_set1_epi16(8);

			const uint32_t numIter = numFanTris >> 2;
			for (uint32_t i = 0; i < numIter; ++i) {
				// { 0, stv + 0, stv + 2, stv + 0, stv + 1, stv + 3, stv + 0, stv + 3 }
				// { stv + 2, 0, stv + 2, stv + 4, stv + 2, stv + 3, stv + 5, stv + 2 }
				// { stv + 5, stv + 4, 0, stv + 4, stv + 6, stv + 4, stv + 5, stv + 7 }
				// { stv + 4, stv + 7, stv + 6, 0, stv + 6, stv + 8, stv + 6, stv + 7 }
				// { stv + 9, stv + 6, stv + 9, stv + 8 }
				const __m128i xmm_id0 = _mm_add_epi16(xmm_stv, xmm_delta0);
				const __m128i xmm_id1 = _mm_add_epi16(xmm_stv, xmm_delta1);
				const __m128i xmm_id2 = _mm_add_epi16(xmm_stv, xmm_delta2);
				const __m128i xmm_id3 = _mm_add_epi16(xmm_stv, xmm_delta3);
				const __m128i xmm_id4 = _mm_add_epi16(xmm_stv, xmm_delta4);

				_mm_storeu_si128((__m128i*)(dstIndex + 0), _mm_insert_epi16(xmm_id0, 0, 0));
				_mm_storeu_si128((__m128i*)(dstIndex + 8), _mm_insert_epi16(xmm_id1, 0, 1));
				_mm_storeu_si128((__m128i*)(dstIndex + 16), _mm_insert_epi16(xmm_id2, 0, 2));
				_mm_storeu_si128((__m128i*)(dstIndex + 24), _mm_insert_epi16(xmm_id3, 0, 3));
				_mm_storel_epi64((__m128i*)(dstIndex + 32), xmm_id4);

				dstIndex += 36;
				xmm_stv = _mm_add_epi16(xmm_stv, xmm_stv_delta);
			}
		}

		{
			static const uint16_t delta0[8] = { 0, 2, 0, 1, 3, 0, 3, 2 };
			static const uint16_t delta1[8] = { 2, 4, 2, 3, 5, 2, 5, 4 };
			const __m128i xmm_delta0 = _mm_loadu_si128((const __m128i*)delta0);

			uint32_t rem = numFanTris & 3;
			if (rem >= 2) {
				const __m128i xmm_delta1 = _mm_loadu_si128((const __m128i*)delta1);
				const __m128i xmm_id0 = _mm_add_epi16(xmm_stv, xmm_delta0);
				const __m128i xmm_id1 = _mm_add_epi16(xmm_stv, xmm_delta1);

				dstIndex[0] = 0;
				_mm_storeu_si128((__m128i*)(dstIndex + 1), xmm_id0);

				dstIndex[9] = 0;
				_mm_storeu_si128((__m128i*)(dstIndex + 10), xmm_id1);

				dstIndex += 18;
				xmm_stv = _mm_add_epi16(xmm_stv, _mm_set1_epi16(4));
				rem -= 2;
			}

			if (rem) {
				const __m128i xmm_id0 = _mm_add_epi16(xmm_stv, xmm_delta0);

				dstIndex[0] = 0;
				_mm_storeu_si128((__m128i*)(dstIndex + 1), xmm_id0);

				dstIndex += 9;
			}
		}

		// Last fringe quad
		const uint16_t lastID = (uint16_t)((numVertices - 1) << 1);
		dstIndex[0] = lastID;
		dstIndex[1] = lastID + 1;
		dstIndex[2] = 1;
		dstIndex[3] = lastID;
		dstIndex[4] = 1;
		dstIndex[5] = 0;

		stroker->m_NumIndices += numDrawIndices;
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = stroker->m_ColorBuffer;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}
#else
void strokerConvexFillAA(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, uint32_t color)
{
	// Determine path orientation by checking the normal of the first triangle
	// WARNING: Might not work in all cases.
	VG_CHECK(numVertices >= 3, "Invalid number of vertices");

	const Vec2* vtx = (const Vec2*)vertexList;

	const float cross = vec2Cross(vec2Sub(vtx[1], vtx[0]), vec2Sub(vtx[2], vtx[0]));

	const float aa = stroker->m_FringeWidth * 0.5f * bx::sign(cross);
	const uint32_t c0 = colorSetAlpha(color, 0);

	const uint32_t numTris =
		(numVertices - 2) + // Triangle fan
		(numVertices * 2); // AA fringes
	const uint32_t numDrawVertices = numVertices * 2; // original polygon point + AA fringe point.
	const uint32_t numDrawIndices = numTris * 3;

	resetGeometry(stroker);

	// Vertex buffer
	{
		expandVB(stroker, numDrawVertices);

		Vec2 d01 = vec2Dir(vtx[numVertices - 1], vtx[0]);

		Vec2* dstPos = stroker->m_PosBuffer;
		for (uint32_t iSegment = 0; iSegment < numVertices; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];
			const Vec2& p2 = vtx[iSegment == numVertices - 1 ? 0 : iSegment + 1];

			const Vec2 d12 = vec2Dir(p1, p2);
			const Vec2 v = calcExtrusionVector(d01, d12);
			const Vec2 v_aa = vec2Scale(v, aa);

			dstPos[0] = vec2Add(p1, v_aa);
			dstPos[1] = vec2Sub(p1, v_aa);
			dstPos += 2;

			d01 = d12;
		}

		const uint32_t colors[2] = { color, c0 };
		vgutil::memset64(stroker->m_ColorBuffer, numVertices, &colors[0]);

		stroker->m_NumVertices += numDrawVertices;
	}

	// Index buffer
	{
		expandIB(stroker, numDrawIndices);

		uint16_t* dstIndex = stroker->m_IndexBuffer;

		// Generate the triangle fan (original polygon)
		const uint32_t numFanTris = numVertices - 2;
		uint16_t secondTriVertex = 2;
		for (uint32_t i = 0; i < numFanTris; ++i) {
			*dstIndex++ = 0;
			*dstIndex++ = secondTriVertex;
			*dstIndex++ = secondTriVertex + 2;
			secondTriVertex += 2;
		}

		// Generate the AA fringes
		uint16_t firstVertexID = 0;
		for (uint32_t i = 0; i < numVertices - 1; ++i) {
			*dstIndex++ = firstVertexID;
			*dstIndex++ = firstVertexID + 1;
			*dstIndex++ = firstVertexID + 3;
			*dstIndex++ = firstVertexID;
			*dstIndex++ = firstVertexID + 3;
			*dstIndex++ = firstVertexID + 2;
			firstVertexID += 2;
		}

		// Last segment
		*dstIndex++ = firstVertexID;
		*dstIndex++ = firstVertexID + 1;
		*dstIndex++ = 1;
		*dstIndex++ = firstVertexID;
		*dstIndex++ = 1;
		*dstIndex++ = 0;

		stroker->m_NumIndices += numDrawIndices;
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = stroker->m_ColorBuffer;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}
#endif

bool strokerConcaveFillBegin(Stroker* stroker)
{
	// Delete old tesselator
	if (stroker->m_Tesselator) {
		tessDeleteTess(stroker->m_Tesselator);
	}

#if VG_CONFIG_LIBTESS2_SCRATCH_BUFFER
	// Initialize the allocator once
	if (!stroker->m_libTessAllocator.m_Buffer) {
		stroker->m_libTessAllocator.m_Capacity = VG_CONFIG_LIBTESS2_SCRATCH_BUFFER;
		stroker->m_libTessAllocator.m_Buffer = (uint8_t*)BX_ALIGNED_ALLOC(stroker->m_Allocator, stroker->m_libTessAllocator.m_Capacity, 16);
	}

	// Reset the allocator.
	stroker->m_libTessAllocator.m_Size = 0;

	// Initialize the tesselator
	TESSalloc allocator;
	allocator.meshEdgeBucketSize = 16;
	allocator.meshVertexBucketSize = 16;
	allocator.meshFaceBucketSize = 16;
	allocator.dictNodeBucketSize = 16;
	allocator.regionBucketSize = 16;
	allocator.extraVertices = 256;
	allocator.userData = &stroker->m_libTessAllocator;
	allocator.memalloc = libtess2Alloc;
	allocator.memfree = libtess2Free;
	allocator.memrealloc = nullptr;
	stroker->m_Tesselator = tessNewTess(&allocator);
#else
	stroker->m_Tesselator = tessNewTess(nullptr);
#endif

	return true;
}

void strokerConcaveFillAddContour(Stroker* stroker, const float* vertexList, uint32_t numVertices)
{
	tessAddContour(stroker->m_Tesselator, 2, vertexList, sizeof(float) * 2, numVertices);
}

bool strokerConcaveFillEnd(Stroker* stroker, Mesh* mesh, FillRule::Enum fillRule)
{
	const int windingRule = fillRule == vg::FillRule::NonZero ? TESS_WINDING_NONZERO : TESS_WINDING_ODD;
	if (!tessTesselate(stroker->m_Tesselator, windingRule, TESS_POLYGONS, 3, 2, nullptr)) {
		return false;
	}

	mesh->m_PosBuffer = tessGetVertices(stroker->m_Tesselator);
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = tessGetElements(stroker->m_Tesselator);
	mesh->m_NumVertices = (uint32_t)tessGetVertexCount(stroker->m_Tesselator);
	mesh->m_NumIndices = (uint32_t)tessGetElementCount(stroker->m_Tesselator) * 3;

	return true;
}

bool strokerConcaveFillEndAA(Stroker* stroker, Mesh* mesh, uint32_t color, FillRule::Enum fillRule)
{
	const int windingRule = fillRule == vg::FillRule::NonZero ? TESS_WINDING_NONZERO : TESS_WINDING_ODD;
	const Color c0 = colorSetAlpha(color, 0);

	uint32_t nextVertexID = 0;
	uint32_t nextIndexID = 0;

	resetGeometry(stroker);

	// Generate fringes
	const float normal[3] = { 0.0f, 0.0f, 1.0f };
	if (!tessTesselate(stroker->m_Tesselator, windingRule, TESS_BOUNDARY_CONTOURS, 1, 2, &normal[0])) {
		return false;
	}
	
	const float* contourVerts = tessGetVertices(stroker->m_Tesselator);
	const TESSindex* contourData = tessGetElements(stroker->m_Tesselator);
	const int numContours = tessGetElementCount(stroker->m_Tesselator);

	for (int i = 0; i < numContours; ++i) {
		const TESSindex firstContourVertexID = contourData[i * 2];
		const TESSindex numContourVertices = contourData[i * 2 + 1];

		// Vertices
		expandVB(stroker, numContourVertices * 2);
		{
			Vec2* vtx = (Vec2*)&contourVerts[firstContourVertexID * 2];
			Vec2 d01 = vec2Dir(vtx[numContourVertices - 1], vtx[0]);
			const float crossSign = bx::sign(vec2Cross(d01, vec2Dir(vtx[0], vtx[1])));
			const float aa = stroker->m_FringeWidth * 0.5f * crossSign;
			const uint32_t inner = crossSign < 0 ? 0 : 1;

			Vec2* dstPos = &stroker->m_PosBuffer[nextVertexID];
			Color* dstColor = &stroker->m_ColorBuffer[nextVertexID];
			for (uint32_t iSegment = 0; iSegment < numContourVertices; ++iSegment) {
				const Vec2& p1 = vtx[iSegment];
				const Vec2& p2 = vtx[iSegment == (uint32_t)(numContourVertices - 1) ? 0 : iSegment + 1];

				const Vec2 d12 = vec2Dir(p1, p2);
				const Vec2 v = calcExtrusionVector(d01, d12);
				const Vec2 v_aa = vec2Scale(v, aa);

				const Vec2 p[2] = {
					vec2Sub(p1, v_aa),
					vec2Add(p1, v_aa)
				};

				// Fringe vertices
				*dstPos++ = p[inner];
				*dstPos++ = p[1 - inner];
				*dstColor++ = color;
				*dstColor++ = c0;

				// Update contour vertex
				vtx[iSegment] = p[inner];

				d01 = d12;
			}

			stroker->m_NumVertices += numContourVertices * 2;
		}

		// Indices
		expandIB(stroker, numContourVertices * 6);
		{
			uint16_t* dstIndex = &stroker->m_IndexBuffer[nextIndexID];

			const uint32_t numSegments = numContourVertices - 1;
			for (uint32_t iSegment = 0; iSegment < numSegments; ++iSegment) {
				const uint16_t id0 = (uint16_t)(nextVertexID + iSegment * 2 + 0);
				const uint16_t id1 = (uint16_t)(nextVertexID + iSegment * 2 + 1);
				const uint16_t id2 = (uint16_t)(nextVertexID + iSegment * 2 + 2);
				const uint16_t id3 = (uint16_t)(nextVertexID + iSegment * 2 + 3);

				dstIndex[0] = id0;
				dstIndex[1] = id2;
				dstIndex[2] = id1;
				dstIndex[3] = id2;
				dstIndex[4] = id3;
				dstIndex[5] = id1;
				dstIndex += 6;
			}

			// Last (closing) segment
			{
				const uint16_t id0 = (uint16_t)(nextVertexID + numSegments * 2 + 0);
				const uint16_t id1 = (uint16_t)(nextVertexID + numSegments * 2 + 1);
				const uint16_t id2 = (uint16_t)(nextVertexID + 0);
				const uint16_t id3 = (uint16_t)(nextVertexID + 1);

				dstIndex[0] = id0;
				dstIndex[1] = id2;
				dstIndex[2] = id1;
				dstIndex[3] = id2;
				dstIndex[4] = id3;
				dstIndex[5] = id1;
			}

			stroker->m_NumIndices += numContourVertices * 6;
		}

		tessAddContour(stroker->m_Tesselator, 2, &contourVerts[firstContourVertexID * 2], sizeof(float) * 2, numContourVertices);

		nextIndexID += numContourVertices * 6;
		nextVertexID += numContourVertices * 2;
	}

	// Generate interior
	if (!tessTesselate(stroker->m_Tesselator, windingRule, TESS_POLYGONS, 3, 2, &normal[0])) {
		return false;
	}

	const uint32_t numTessVertices = (uint32_t)tessGetVertexCount(stroker->m_Tesselator);
	expandVB(stroker, numTessVertices);
	{
		const float* tessVertices = tessGetVertices(stroker->m_Tesselator);
		bx::memCopy(&stroker->m_PosBuffer[nextVertexID], tessVertices, sizeof(Vec2) * numTessVertices);
		vgutil::memset32(&stroker->m_ColorBuffer[nextVertexID], numTessVertices, &color);
		stroker->m_NumVertices += numTessVertices;
	}

	const uint32_t numTessIndices = tessGetElementCount(stroker->m_Tesselator) * 3;
	expandIB(stroker, numTessIndices);
	{
		vgutil::batchTransformDrawIndices(tessGetElements(stroker->m_Tesselator), numTessIndices, &stroker->m_IndexBuffer[nextIndexID], (uint16_t)nextVertexID);
		stroker->m_NumIndices += numTessIndices;
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = stroker->m_ColorBuffer;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;

	return true;
}

//////////////////////////////////////////////////////////////////////////
// Templates
//
template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void polylineStroke(Stroker* stroker, Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth)
{
	const uint32_t numSegments = numPathVertices - (_Closed ? 0 : 1);
	const float hsw = strokeWidth * 0.5f;
	const float da = bx::acos((stroker->m_Scale * hsw) / ((stroker->m_Scale * hsw) + stroker->m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2u, (uint32_t)bx::ceil(bx::kPi / da));

	resetGeometry(stroker);

	Vec2 d01;
	uint16_t prevSegmentLeftID = 0xFFFF;
	uint16_t prevSegmentRightID = 0xFFFF;
	uint16_t firstSegmentLeftID = 0xFFFF;
	uint16_t firstSegmentRightID = 0xFFFF;
	if (!_Closed) {
		// First segment of an open path
		const Vec2& p0 = vtx[0];
		const Vec2& p1 = vtx[1];

		d01 = vec2Dir(p0, p1);

		const Vec2 l01 = vec2PerpCCW(d01);

		if (_LineCap == LineCap::Butt) {
			const Vec2 l01_hsw = vec2Scale(l01, hsw);

			Vec2 p[2] = {
				vec2Add(p0, l01_hsw),
				vec2Sub(p0, l01_hsw)
			};

			expandVB(stroker, 2);
			addPos<2>(stroker, &p[0]);

			prevSegmentLeftID = 0;
			prevSegmentRightID = 1;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 d01_hsw = vec2Scale(d01, hsw);

			Vec2 p[2] = {
				vec2Add(p0, vec2Sub(l01_hsw, d01_hsw)),
				vec2Sub(p0, vec2Add(l01_hsw, d01_hsw))
			};

			expandVB(stroker, 2);
			addPos<2>(stroker, &p[0]);

			prevSegmentLeftID = 0;
			prevSegmentRightID = 1;
		} else if (_LineCap == LineCap::Round) {
			expandVB(stroker, numPointsHalfCircle);

			const float startAngle = bx::atan2(l01.y, l01.x);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle + i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::cos(a);
				float sa = bx::sin(a);

				Vec2 p = { p0.x + ca * hsw, p0.y + sa * hsw };

				addPos<1>(stroker, &p);
			}

			expandIB(stroker, (numPointsHalfCircle - 2) * 3);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				uint16_t id[3] = { 0, (uint16_t)(i + 1), (uint16_t)(i + 2) };
				addIndices<3>(stroker, &id[0]);
			}

			prevSegmentLeftID = 0;
			prevSegmentRightID = (uint16_t)(numPointsHalfCircle - 1);
		} else {
			VG_CHECK(false, "Unknown line cap type");
		}
	} else {
		d01 = vec2Dir(vtx[numPathVertices - 1], vtx[0]);
	}

	const uint32_t firstSegmentID = _Closed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		const Vec2 d12 = vec2Dir(p1, p2);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_hsw = vec2Scale(v, hsw);

		// Check which one of the points is the inner corner.
		float leftPointProjDist = d12.x * v_hsw.x + d12.y * v_hsw.y;
		if (leftPointProjDist >= 0.0f) {
			// The left point is the inner corner.
			const Vec2 innerCorner = vec2Add(p1, v_hsw);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)stroker->m_NumVertices;

				Vec2 p[2] = {
					innerCorner,
					vec2Sub(p1, v_hsw)
				};

				expandVB(stroker, 2);
				addPos<2>(stroker, &p[0]);

				if (prevSegmentLeftID != 0xFFFF) {
					VG_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstVertexID + 1), firstVertexID
					};

					expandIB(stroker, 6);
					addIndices<6>(stroker, &id[0]);
				} else {
					firstSegmentLeftID = firstVertexID;
					firstSegmentRightID = firstVertexID + 1;
				}

				prevSegmentLeftID = firstVertexID;
				prevSegmentRightID = firstVertexID + 1;
			} else {
				const Vec2 r01 = vec2PerpCW(d01);
				const Vec2 r12 = vec2PerpCW(d12);

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = bx::atan2(r01.y, r01.x);
					a12 = bx::atan2(r12.y, r12.x);
					if (a12 < a01) {
						a12 += bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a12 - a01) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				Vec2 p[3] = {
					innerCorner,
					vec2Add(p1, vec2Scale(r01, hsw)),
					vec2Add(p1, vec2Scale(r12, hsw))
				};

				uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;
				expandVB(stroker, numArcPoints + 2);
				addPos<2>(stroker, &p[0]);
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;
					float ca = bx::cos(a);
					float sa = bx::sin(a);

					Vec2 p = { p1.x + hsw * ca, p1.y + hsw * sa };

					addPos<1>(stroker, &p);
				}
				addPos<1>(stroker, &p[2]);

				if (prevSegmentLeftID != 0xFFFF) {
					VG_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID
					};

					expandIB(stroker, 6);
					addIndices<6>(stroker, &id[0]);
				} else {
					firstSegmentLeftID = firstFanVertexID;
					firstSegmentRightID = firstFanVertexID + 1;
				}

				// Generate the triangle fan.
				expandIB(stroker, numArcPoints * 3);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					const uint16_t idBase = firstFanVertexID + (uint16_t)iArcPoint;
					uint16_t id[3] = {
						firstFanVertexID, (uint16_t)(idBase + 1), (uint16_t)(idBase + 2)
					};
					addIndices<3>(stroker, &id[0]);
				}

				prevSegmentLeftID = firstFanVertexID;
				prevSegmentRightID = firstFanVertexID + (uint16_t)numArcPoints + 1;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 innerCorner = vec2Sub(p1, v_hsw);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)stroker->m_NumVertices;

				Vec2 p[2] = {
					innerCorner,
					vec2Add(p1, v_hsw),
				};

				expandVB(stroker, 2);
				addPos<2>(stroker, &p[0]);

				if (prevSegmentLeftID != 0xFFFF) {
					VG_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, firstVertexID,
						prevSegmentLeftID, firstVertexID, (uint16_t)(firstVertexID + 1)
					};

					expandIB(stroker, 6);
					addIndices<6>(stroker, &id[0]);
				} else {
					firstSegmentLeftID = firstVertexID + 1;
					firstSegmentRightID = firstVertexID;
				}

				prevSegmentLeftID = firstVertexID + 1;
				prevSegmentRightID = firstVertexID;
			} else {
				const Vec2 l01 = vec2PerpCCW(d01);
				const Vec2 l12 = vec2PerpCCW(d12);

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = bx::atan2(l01.y, l01.x);
					a12 = bx::atan2(l12.y, l12.x);
					if (a12 > a01) {
						a12 -= bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a01 - a12) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				Vec2 p[3] = {
					innerCorner,
					vec2Add(p1, vec2Scale(l01, hsw)),
					vec2Add(p1, vec2Scale(l12, hsw))
				};

				uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;
				expandVB(stroker, numArcPoints + 2);
				addPos<2>(stroker, &p[0]);
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;
					float ca = bx::cos(a);
					float sa = bx::sin(a);

					Vec2 p = { p1.x + hsw * ca, p1.y + hsw * sa };
					addPos<1>(stroker, &p);
				}
				addPos<1>(stroker, &p[2]);

				if (prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF) {
					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, firstFanVertexID,
						prevSegmentLeftID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(stroker, 6);
					addIndices<6>(stroker, &id[0]);
				} else {
					firstSegmentLeftID = firstFanVertexID + 1;
					firstSegmentRightID = firstFanVertexID;
				}

				expandIB(stroker, numArcPoints * 3);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					const uint16_t idBase = firstFanVertexID + (uint16_t)iArcPoint;
					uint16_t id[3] = {
						firstFanVertexID, (uint16_t)(idBase + 2), (uint16_t)(idBase + 1)
					};
					addIndices<3>(stroker, &id[0]);
				}

				prevSegmentLeftID = firstFanVertexID + (uint16_t)numArcPoints + 1;
				prevSegmentRightID = firstFanVertexID;
			}
		}

		d01 = d12;
	}

	if (!_Closed) {
		// Last segment of an open path
		const Vec2& p1 = vtx[numPathVertices - 1];

		const Vec2 l01 = vec2PerpCCW(d01);

		if (_LineCap == LineCap::Butt) {
			const uint16_t curSegmentLeftID = (uint16_t)stroker->m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);

			Vec2 p[2] = {
				vec2Add(p1, l01_hsw),
				vec2Sub(p1, l01_hsw)
			};

			expandVB(stroker, 2);
			addPos<2>(stroker, &p[0]);

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + 1), curSegmentLeftID
			};

			expandIB(stroker, 6);
			addIndices<6>(stroker, &id[0]);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftID = (uint16_t)stroker->m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 d01_hsw = vec2Scale(d01, hsw);

			Vec2 p[2] = {
				vec2Add(p1, vec2Add(l01_hsw, d01_hsw)),
				vec2Sub(p1, vec2Sub(l01_hsw, d01_hsw))
			};

			expandVB(stroker, 2);
			addPos<2>(stroker, &p[0]);

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + 1), curSegmentLeftID
			};

			expandIB(stroker, 6);
			addIndices<6>(stroker, &id[0]);
		} else if (_LineCap == LineCap::Round) {
			expandVB(stroker, numPointsHalfCircle);

			const uint16_t curSegmentLeftID = (uint16_t)stroker->m_NumVertices;
			const float startAngle = bx::atan2(l01.y, l01.x);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle - i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::cos(a);
				float sa = bx::sin(a);

				Vec2 p = { p1.x + ca * hsw, p1.y + sa * hsw };

				addPos<1>(stroker, &p);
			}

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1)),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1)), curSegmentLeftID
			};

			expandIB(stroker, 6 + (numPointsHalfCircle - 2) * 3);
			addIndices<6>(stroker, &id[0]);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)i;
				uint16_t id[3] = {
					curSegmentLeftID, (uint16_t)(idBase + 2), (uint16_t)(idBase + 1)
				};
				addIndices<3>(stroker, &id[0]);
			}
		}
	} else {
		// Generate the first segment quad. 
		uint16_t id[6] = {
			prevSegmentLeftID, prevSegmentRightID, firstSegmentRightID,
			prevSegmentLeftID, firstSegmentRightID, firstSegmentLeftID
		};

		expandIB(stroker, 6);
		addIndices<6>(stroker, &id[0]);
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}

template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void polylineStrokeAA(Stroker* stroker, Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth, Color color)
{
	const uint32_t numSegments = numPathVertices - (_Closed ? 0 : 1);
	const uint32_t c0 = colorSetAlpha(color, 0);
	const uint32_t c0_c_c_c0[4] = { c0, color, color, c0 };
	const float hsw = (strokeWidth - stroker->m_FringeWidth) * 0.5f;
	const float hsw_aa = hsw + stroker->m_FringeWidth;
	const float da = bx::acos((stroker->m_Scale * hsw) / ((stroker->m_Scale * hsw) + stroker->m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2u, (uint32_t)bx::ceil(bx::kPi / da));

	resetGeometry(stroker);

	Vec2 d01;
	uint16_t prevSegmentLeftID = 0xFFFF;
	uint16_t prevSegmentLeftAAID = 0xFFFF;
	uint16_t prevSegmentRightID = 0xFFFF;
	uint16_t prevSegmentRightAAID = 0xFFFF;
	uint16_t firstSegmentLeftID = 0xFFFF;
	uint16_t firstSegmentLeftAAID = 0xFFFF;
	uint16_t firstSegmentRightID = 0xFFFF;
	uint16_t firstSegmentRightAAID = 0xFFFF;

	if (!_Closed) {
		// First segment of an open path
		const Vec2& p0 = vtx[0];
		const Vec2& p1 = vtx[1];

		d01 = vec2Dir(p0, p1);

		const Vec2 l01 = vec2PerpCCW(d01);

		if (_LineCap == LineCap::Butt) {
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);
			const Vec2 d01_aa = vec2Scale(d01, stroker->m_FringeWidth);

			Vec2 p[4] = {
				vec2Add(p0, vec2Sub(l01_hsw_aa, d01_aa)),
				vec2Add(p0, l01_hsw),
				vec2Sub(p0, l01_hsw),
				vec2Sub(p0, vec2Add(l01_hsw_aa, d01_aa))
			};

			expandVB(stroker, 4);
			addPosColor<4>(stroker, &p[0], &c0_c_c_c0[0]);

			uint16_t id[6] = {
				0, 2, 1,
				0, 3, 2
			};
			expandIB(stroker, 6);
			addIndices<6>(stroker, &id[0]);

			prevSegmentLeftAAID = 0;
			prevSegmentLeftID = 1;
			prevSegmentRightID = 2;
			prevSegmentRightAAID = 3;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 d01_hsw = vec2Scale(d01, hsw);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);
			const Vec2 d01_hsw_aa = vec2Scale(d01, hsw_aa);

			Vec2 p[4] = {
				vec2Add(p0, vec2Sub(l01_hsw_aa, d01_hsw_aa)),
				vec2Add(p0, vec2Sub(l01_hsw, d01_hsw)),
				vec2Sub(p0, vec2Add(l01_hsw, d01_hsw)),
				vec2Sub(p0, vec2Add(l01_hsw_aa, d01_hsw_aa))
			};

			expandVB(stroker, 4);
			addPosColor<4>(stroker, &p[0], &c0_c_c_c0[0]);

			uint16_t id[6] = {
				0, 2, 1,
				0, 3, 2
			};
			expandIB(stroker, 6);
			addIndices<6>(stroker, &id[0]);

			prevSegmentLeftAAID = 0;
			prevSegmentLeftID = 1;
			prevSegmentRightID = 2;
			prevSegmentRightAAID = 3;
		} else if (_LineCap == LineCap::Round) {
			const float startAngle = bx::atan2(l01.y, l01.x);
			expandVB(stroker, numPointsHalfCircle << 1);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle + i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::cos(a);
				float sa = bx::sin(a);

				Vec2 p[2] = {
					{ p0.x + ca * hsw, p0.y + sa * hsw },
					{ p0.x + ca * hsw_aa, p0.y + sa * hsw_aa }
				};

				addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
			}

			// Generate indices for the triangle fan
			expandIB(stroker, numPointsHalfCircle * 9 - 12);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				uint16_t id[3] = {
					0,
					(uint16_t)((i << 1) + 2),
					(uint16_t)((i << 1) + 4)
				};
				addIndices<3>(stroker, &id[0]);
			}

			// Generate indices for the AA quads
			for (uint32_t i = 0; i < numPointsHalfCircle - 1; ++i) {
				const uint16_t idBase = (uint16_t)(i << 1);
				uint16_t id[6] = {
					idBase, (uint16_t)(idBase + 1), (uint16_t)(idBase + 3),
					idBase, (uint16_t)(idBase + 3), (uint16_t)(idBase + 2)
				};
				addIndices<6>(stroker, &id[0]);
			}

			prevSegmentLeftAAID = 1;
			prevSegmentLeftID = 0;
			prevSegmentRightID = (uint16_t)((numPointsHalfCircle - 1) * 2);
			prevSegmentRightAAID = (uint16_t)((numPointsHalfCircle - 1) * 2 + 1);
		} else {
			VG_CHECK(false, "Unknown line cap type");
		}
	} else {
		d01	= vec2Dir(vtx[numPathVertices - 1], vtx[0]);
	}

	const uint32_t firstSegmentID = _Closed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		const Vec2 d12 = vec2Dir(p1, p2);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_hsw_aa = vec2Scale(v, hsw_aa);

		// Check which one of the points is the inner corner.
		float leftPointAAProjDist = d12.x * v_hsw_aa.x + d12.y * v_hsw_aa.y;
		if (leftPointAAProjDist >= 0.0f) {
			// The left point is the inner corner.
			const Vec2 v_hsw = vec2Scale(v, hsw);
			const Vec2 innerCornerAA = vec2Add(p1, v_hsw_aa);
			const Vec2 innerCorner = vec2Add(p1, v_hsw);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)stroker->m_NumVertices;

				Vec2 p[4] = {
					innerCornerAA,
					innerCorner,
					vec2Sub(p1, v_hsw),
					vec2Sub(p1, v_hsw_aa)
				};

				expandVB(stroker, 4);
				addPosColor<4>(stroker, &p[0], &c0_c_c_c0[0]);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstVertexID + 1), firstVertexID,
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstVertexID + 2),
						prevSegmentLeftID, (uint16_t)(firstVertexID + 2), (uint16_t)(firstVertexID + 1),
						prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(firstVertexID + 3),
						prevSegmentRightID, (uint16_t)(firstVertexID + 3), (uint16_t)(firstVertexID + 2)
					};

					expandIB(stroker, 18);
					addIndices<18>(stroker, &id[0]);
				} else {
					VG_CHECK(_Closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstVertexID; // 0
					firstSegmentLeftID = firstVertexID + 1; // 1
					firstSegmentRightID = firstVertexID + 2; // 2
					firstSegmentRightAAID = firstVertexID + 3; // 3
				}

				prevSegmentLeftAAID = firstVertexID;
				prevSegmentLeftID = firstVertexID + 1;
				prevSegmentRightID = firstVertexID + 2;
				prevSegmentRightAAID = firstVertexID + 3;
			} else {
				const Vec2 r01 = vec2PerpCW(d01);
				const Vec2 r12 = vec2PerpCW(d12);

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = bx::atan2(r01.y, r01.x);
					a12 = bx::atan2(r12.y, r12.x);
					if (a12 < a01) {
						a12 += bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a12 - a01) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				const uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;
				expandVB(stroker, numArcPoints * 2 + 4);

				Vec2 p[2] = {
					innerCornerAA,
					innerCorner
				};
				addPosColor<2>(stroker, &p[0], &c0_c_c_c0[0]);

				// First arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(r01, hsw)),
						vec2Add(p1, vec2Scale(r01, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::abs(vec2Dot(r01, r12));
						p[0] = vec2Sub(p[0], vec2Scale(d01, (cosAngle * stroker->m_FringeWidth)));
					}

					addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
				}

				// Middle arc vertices
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;

					const Vec2 arcPointDir = { bx::cos(a), bx::sin(a) };

					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(arcPointDir, hsw)),
						vec2Add(p1, vec2Scale(arcPointDir, hsw_aa))
					};

					addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
				}

				// Last arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(r12, hsw)),
						vec2Add(p1, vec2Scale(r12, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::abs(vec2Dot(r01, r12));
						p[0] = vec2Add(p[0], vec2Scale(d12, (cosAngle * stroker->m_FringeWidth)));
					}

					addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
				}

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID,
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 1),
						prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(firstFanVertexID + 3),
						prevSegmentRightID, (uint16_t)(firstFanVertexID + 3), (uint16_t)(firstFanVertexID + 2)
					};

					expandIB(stroker, 18);
					addIndices<18>(stroker, &id[0]);
				} else {
					VG_CHECK(_Closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstFanVertexID; // 0
					firstSegmentLeftID = firstFanVertexID + 1; // 1
					firstSegmentRightID = firstFanVertexID + 2; // 2
					firstSegmentRightAAID = firstFanVertexID + 3; // 3
				}

				// Generate the slice.
				uint16_t arcID = firstFanVertexID + 2;
				expandIB(stroker, numArcPoints * 9);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					uint16_t id[9] = {
						(uint16_t)(firstFanVertexID + 1), arcID, (uint16_t)(arcID + 2),
						arcID, (uint16_t)(arcID + 1), (uint16_t)(arcID + 3),
						arcID, (uint16_t)(arcID + 3), (uint16_t)(arcID + 2)
					};
					addIndices<9>(stroker, &id[0]);

					arcID += 2;
				}

				prevSegmentLeftAAID = firstFanVertexID;
				prevSegmentLeftID = firstFanVertexID + 1;
				prevSegmentRightID = arcID;
				prevSegmentRightAAID = arcID + 1;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 v_hsw = vec2Scale(v, hsw);
			const Vec2 innerCornerAA = vec2Sub(p1, v_hsw_aa);
			const Vec2 innerCorner = vec2Sub(p1, v_hsw);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;

				Vec2 p[4] = {
					innerCornerAA,
					innerCorner,
					vec2Add(p1, v_hsw),
					vec2Add(p1, v_hsw_aa)
				};

				expandVB(stroker, 4);
				addPosColor<4>(stroker, &p[0], &c0_c_c_c0[0]);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3),
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentRightID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentRightID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(stroker, 18);
					addIndices<18>(stroker, &id[0]);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 3;
					firstSegmentLeftID = firstFanVertexID + 2;
					firstSegmentRightID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				prevSegmentLeftAAID = firstFanVertexID + 3;
				prevSegmentLeftID = firstFanVertexID + 2;
				prevSegmentRightID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID;
			} else {
				const Vec2 l01 = vec2PerpCCW(d01);
				const Vec2 l12 = vec2PerpCCW(d12);

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = bx::atan2(l01.y, l01.x);
					a12 = bx::atan2(l12.y, l12.x);
					if (a12 > a01) {
						a12 -= bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a01 - a12) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				const uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;
				expandVB(stroker, numArcPoints * 2 + 4);

				Vec2 p[2] = {
					innerCornerAA,
					innerCorner
				};
				addPosColor<2>(stroker, &p[0], &c0_c_c_c0[0]);

				// First arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(l01, hsw)),
						vec2Add(p1, vec2Scale(l01, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::abs(vec2Dot(l01, l12));
						p[0] = vec2Sub(p[0], vec2Scale(d01, (cosAngle * stroker->m_FringeWidth)));
					}

					addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
				}

				// Middle arc vertices
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;

					const Vec2 arcPointDir = { bx::cos(a), bx::sin(a) };

					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(arcPointDir, hsw)),
						vec2Add(p1, vec2Scale(arcPointDir, hsw_aa))
					};

					addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
				}

				// Last arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(l12, hsw)),
						vec2Add(p1, vec2Scale(l12, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::abs(vec2Dot(l01, l12));
						p[0] = vec2Add(p[0], vec2Scale(d12, (cosAngle * stroker->m_FringeWidth)));
					}

					addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
				}

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3),
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentRightID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentRightID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(stroker, 18);
					addIndices<18>(stroker, &id[0]);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 3;
					firstSegmentLeftID = firstFanVertexID + 2;
					firstSegmentRightID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				// Generate the slice.
				uint16_t arcID = firstFanVertexID + 2;
				expandIB(stroker, numArcPoints * 9);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					uint16_t id[9] = {
						(uint16_t)(firstFanVertexID + 1), (uint16_t)(arcID + 2), arcID,
						arcID, (uint16_t)(arcID + 3), (uint16_t)(arcID + 1),
						arcID, (uint16_t)(arcID + 2), (uint16_t)(arcID + 3)
					};
					addIndices<9>(stroker, &id[0]);

					arcID += 2;
				}

				prevSegmentLeftAAID = arcID + 1;
				prevSegmentLeftID = arcID;
				prevSegmentRightID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID;
			}
		}

		d01 = d12;
	}

	if (!_Closed) {
		// Last segment of an open path
		const Vec2& p1 = vtx[numPathVertices - 1];

		const Vec2 l01 = vec2PerpCCW(d01);

		if (_LineCap == LineCap::Butt) {
			const uint16_t curSegmentLeftAAID = (uint16_t)stroker->m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);
			const Vec2 d01_aa = vec2Scale(d01, stroker->m_FringeWidth);

			Vec2 p[4] = {
				vec2Add(p1, vec2Add(l01_hsw_aa, d01_aa)),
				vec2Add(p1, l01_hsw),
				vec2Sub(p1, l01_hsw),
				vec2Sub(p1, vec2Sub(l01_hsw_aa, d01_aa))
			};

			expandVB(stroker, 4);
			addPosColor<4>(stroker, &p[0], &c0_c_c_c0[0]);

			uint16_t id[24] = {
				prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 3),
				prevSegmentRightID, (uint16_t)(curSegmentLeftAAID + 3), (uint16_t)(curSegmentLeftAAID + 2),
				curSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), (uint16_t)(curSegmentLeftAAID + 2),
				curSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 3)
			};

			expandIB(stroker, 24);
			addIndices<24>(stroker, &id[0]);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftAAID = (uint16_t)stroker->m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 d01_hsw = vec2Scale(d01, hsw);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);
			const Vec2 d01_hsw_aa = vec2Scale(d01, hsw_aa);

			Vec2 p[4] = {
				vec2Add(p1, vec2Add(l01_hsw_aa, d01_hsw_aa)),
				vec2Add(p1, vec2Add(l01_hsw, d01_hsw)),
				vec2Sub(p1, vec2Sub(l01_hsw, d01_hsw)),
				vec2Sub(p1, vec2Sub(l01_hsw_aa, d01_hsw_aa))
			};

			expandVB(stroker, 4);
			addPosColor<4>(stroker, &p[0], &c0_c_c_c0[0]);

			uint16_t id[24] = {
				prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 3),
				prevSegmentRightID, (uint16_t)(curSegmentLeftAAID + 3), (uint16_t)(curSegmentLeftAAID + 2),
				curSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), (uint16_t)(curSegmentLeftAAID + 2),
				curSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 3)
			};

			expandIB(stroker, 24);
			addIndices<24>(stroker, &id[0]);
		} else if (_LineCap == LineCap::Round) {
			const uint16_t curSegmentLeftID = (uint16_t)stroker->m_NumVertices;
			const float startAngle = bx::atan2(l01.y, l01.x);

			expandVB(stroker, numPointsHalfCircle * 2);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle - i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::cos(a);
				float sa = bx::sin(a);

				Vec2 p[2] = {
					{ p1.x + ca * hsw, p1.y + sa * hsw },
					{ p1.x + ca * hsw_aa, p1.y + sa * hsw_aa }
				};

				addPosColor<2>(stroker, &p[0], &c0_c_c_c0[2]);
			}

			uint16_t id[18] = {
				prevSegmentLeftAAID, prevSegmentLeftID, curSegmentLeftID,
				prevSegmentLeftAAID, curSegmentLeftID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2), curSegmentLeftID,
				prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2 + 1),
				prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2 + 1), (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2)
			};

			expandIB(stroker, 18);
			addIndices<18>(stroker, &id[0]);

			// Generate indices for the triangle fan
			expandIB(stroker, (numPointsHalfCircle - 2) * 3);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)(i << 1);
				uint16_t id[3] = {
					curSegmentLeftID,
					(uint16_t)(idBase + 4),
					(uint16_t)(idBase + 2)
				};
				addIndices<3>(stroker, &id[0]);
			}

			// Generate indices for the AA quads
			expandIB(stroker, (numPointsHalfCircle - 1) * 6);
			for (uint32_t i = 0; i < numPointsHalfCircle - 1; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)(i << 1);
				uint16_t id[6] = {
					idBase, (uint16_t)(idBase + 3), (uint16_t)(idBase + 1),
					idBase, (uint16_t)(idBase + 2), (uint16_t)(idBase + 3)
				};
				addIndices<6>(stroker, &id[0]);
			}
		}
	} else {
		VG_CHECK(firstSegmentLeftAAID != 0xFFFF && firstSegmentLeftID != 0xFFFF && firstSegmentRightID != 0xFFFF && firstSegmentRightAAID != 0xFFFF, "Invalid first segment");

		uint16_t id[18] = {
			prevSegmentLeftAAID, prevSegmentLeftID, firstSegmentLeftID,
			prevSegmentLeftAAID, firstSegmentLeftID, firstSegmentLeftAAID,
			prevSegmentLeftID, prevSegmentRightID, firstSegmentRightID,
			prevSegmentLeftID, firstSegmentRightID, firstSegmentLeftID,
			prevSegmentRightID, prevSegmentRightAAID, firstSegmentRightAAID,
			prevSegmentRightID, firstSegmentRightAAID, firstSegmentRightID
		};

		expandIB(stroker, 18);
		addIndices<18>(stroker, &id[0]);
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = stroker->m_ColorBuffer;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}

template<LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void polylineStrokeAAThin(Stroker* stroker, Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, Color color, bool closed)
{
	const uint32_t numSegments = numPathVertices - (closed ? 0 : 1);
	const uint32_t c0 = colorSetAlpha(color, 0);
	const uint32_t c0_c_c0_c0[4] = { c0, color, c0, c0 };
	const float hsw_aa = stroker->m_FringeWidth;

	resetGeometry(stroker);

	Vec2 d01;
	uint16_t prevSegmentLeftAAID = 0xFFFF;
	uint16_t prevSegmentMiddleID = 0xFFFF;
	uint16_t prevSegmentRightAAID = 0xFFFF;

	uint16_t firstSegmentLeftAAID = 0xFFFF;
	uint16_t firstSegmentMiddleID = 0xFFFF;
	uint16_t firstSegmentRightAAID = 0xFFFF;

	if (!closed) {
		// First segment of an open path
		const Vec2& p0 = vtx[0];
		const Vec2& p1 = vtx[1];

		d01 = vec2Dir(p0, p1);

		const Vec2 l01 = vec2PerpCCW(d01);

		if (_LineCap == LineCap::Butt) {
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);

			Vec2 p[3] = {
				vec2Add(p0, l01_hsw_aa),
				p0,
				vec2Sub(p0, l01_hsw_aa)
			};

			expandVB(stroker, 3);
			addPosColor<3>(stroker, &p[0], &c0_c_c0_c0[0]);

			prevSegmentLeftAAID = 0;
			prevSegmentMiddleID = 1;
			prevSegmentRightAAID = 2;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 d01_hsw_aa = vec2Scale(d01, hsw_aa);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);

			Vec2 p[4] = {
				vec2Add(p0, vec2Sub(l01_hsw_aa, d01_hsw_aa)),
				p0,
				vec2Sub(p0, vec2Add(l01_hsw_aa, d01_hsw_aa))
			};

			expandVB(stroker, 3);
			addPosColor<3>(stroker, &p[0], &c0_c_c0_c0[0]);

			prevSegmentLeftAAID = 0;
			prevSegmentMiddleID = 1;
			prevSegmentRightAAID = 2;
		} else if (_LineCap == LineCap::Round) {
			VG_CHECK(false, "Round caps not implemented for thin strokes.");
		} else {
			VG_CHECK(false, "Unknown line cap type");
		}
	} else {
		d01 = vec2Dir(vtx[numPathVertices - 1], vtx[0]);
	}

	const uint32_t firstSegmentID = closed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		const Vec2 d12 = vec2Dir(p1, p2);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_hsw_aa = vec2Scale(v, hsw_aa);

		// Check which one of the points is the inner corner.
		float leftPointAAProjDist = d12.x * v_hsw_aa.x + d12.y * v_hsw_aa.y;
		if (leftPointAAProjDist >= 0.0f) {
			// The left point is the inner corner.
			const Vec2 innerCorner = vec2Add(p1, v_hsw_aa);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)stroker->m_NumVertices;

				Vec2 p[3] = {
					innerCorner,
					p1,
					vec2Sub(p1, v_hsw_aa)
				};

				expandVB(stroker, 3);
				addPosColor<3>(stroker, &p[0], &c0_c_c0_c0[0]);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstVertexID + 1), firstVertexID,
						prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(firstVertexID + 2),
						prevSegmentMiddleID, (uint16_t)(firstVertexID + 2), (uint16_t)(firstVertexID + 1)
					};

					expandIB(stroker, 12);
					addIndices<12>(stroker, id);
				} else {
					VG_CHECK(closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstVertexID;
					firstSegmentMiddleID = firstVertexID + 1;
					firstSegmentRightAAID = firstVertexID + 2;
				}

				prevSegmentLeftAAID = firstVertexID;
				prevSegmentMiddleID = firstVertexID + 1;
				prevSegmentRightAAID = firstVertexID + 2;
			} else {
				VG_CHECK(_LineJoin != LineJoin::Round, "Round joins not implemented for thin strokes.");
				const Vec2 r01 = vec2PerpCW(d01);
				const Vec2 r12 = vec2PerpCW(d12);

				Vec2 p[4] = {
					innerCorner,
					p1,
					vec2Add(p1, vec2Scale(r01, hsw_aa)),
					vec2Add(p1, vec2Scale(r12, hsw_aa))
				};

				const uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;
				expandVB(stroker, 4);
				addPosColor<4>(stroker, &p[0], &c0_c_c0_c0[0]);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID,
						prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(stroker, 12);
					addIndices<12>(stroker, id);
				} else {
					VG_CHECK(closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstFanVertexID;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 2;
				}

				uint16_t id[3] = {
					(uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3)
				};
				expandIB(stroker, 3);
				addIndices<3>(stroker, id);

				prevSegmentLeftAAID = firstFanVertexID;
				prevSegmentMiddleID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID + 3;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 innerCorner = vec2Sub(p1, v_hsw_aa);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;

				Vec2 p[3] = {
					innerCorner,
					p1,
					vec2Add(p1, v_hsw_aa)
				};

				expandVB(stroker, 3);
				addPosColor<3>(stroker, &p[0], &c0_c_c0_c0[0]);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentMiddleID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(stroker, 12);
					addIndices<12>(stroker, id);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 2;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				prevSegmentLeftAAID = firstFanVertexID + 2;
				prevSegmentMiddleID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID;
			} else {
				const Vec2 l01 = vec2PerpCCW(d01);
				const Vec2 l12 = vec2PerpCCW(d12);

				Vec2 p[4] = {
					innerCorner,
					p1,
					vec2Add(p1, vec2Scale(l01, hsw_aa)),
					vec2Add(p1, vec2Scale(l12, hsw_aa))
				};

				const uint16_t firstFanVertexID = (uint16_t)stroker->m_NumVertices;
				expandVB(stroker, 4);
				addPosColor<4>(stroker, &p[0], &c0_c_c0_c0[0]);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentMiddleID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(stroker, 12);
					addIndices<12>(stroker, id);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 2;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				uint16_t id[3] = {
					(uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 3), (uint16_t)(firstFanVertexID + 2)
				};
				expandIB(stroker, 3);
				addIndices<3>(stroker, id);

				prevSegmentLeftAAID = firstFanVertexID + 3;
				prevSegmentMiddleID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID;
			}
		}

		d01 = d12;
	}

	if (!closed) {
		// Last segment of an open path
		const Vec2& p1 = vtx[numPathVertices - 1];

		const Vec2 l01 = vec2PerpCCW(d01);

		if (_LineCap == LineCap::Butt) {
			const uint16_t curSegmentLeftAAID = (uint16_t)stroker->m_NumVertices;
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);

			Vec2 p[3] = {
				vec2Add(p1, l01_hsw_aa),
				p1,
				vec2Sub(p1, l01_hsw_aa)
			};

			expandVB(stroker, 3);
			addPosColor<3>(stroker, &p[0], &c0_c_c0_c0[0]);

			uint16_t id[12] = {
				prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1)
			};

			expandIB(stroker, 12);
			addIndices<12>(stroker, id);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftAAID = (uint16_t)stroker->m_NumVertices;
			const Vec2 d01_hsw = vec2Scale(d01, hsw_aa);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);

			Vec2 p[3] = {
				vec2Add(p1, vec2Add(l01_hsw_aa, d01_hsw)),
				p1,
				vec2Sub(p1, vec2Sub(l01_hsw_aa, d01_hsw))
			};

			expandVB(stroker, 3);
			addPosColor<3>(stroker, &p[0], &c0_c_c0_c0[0]);

			uint16_t id[12] = {
				prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1)
			};

			expandIB(stroker, 12);
			addIndices<12>(stroker, id);
		} else if (_LineCap == LineCap::Round) {
			VG_CHECK(false, "Round caps not implemented for thin strokes.");
		}
	} else {
		VG_CHECK(firstSegmentLeftAAID != 0xFFFF && firstSegmentMiddleID != 0xFFFF && firstSegmentRightAAID != 0xFFFF, "Invalid first segment");

		uint16_t id[12] = {
			prevSegmentLeftAAID, prevSegmentMiddleID, firstSegmentMiddleID,
			prevSegmentLeftAAID, firstSegmentMiddleID, firstSegmentLeftAAID,
			prevSegmentMiddleID, prevSegmentRightAAID, firstSegmentRightAAID,
			prevSegmentMiddleID, firstSegmentRightAAID, firstSegmentMiddleID
		};

		expandIB(stroker, 12);
		addIndices<12>(stroker, id);
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = stroker->m_ColorBuffer;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}

inline static void resetGeometry(Stroker* stroker)
{
	stroker->m_NumVertices = 0;
	stroker->m_NumIndices = 0;
}

static void reallocVB(Stroker* stroker, uint32_t n)
{
	stroker->m_VertexCapacity += n;
	stroker->m_PosBuffer = (Vec2*)BX_ALIGNED_REALLOC(stroker->m_Allocator, stroker->m_PosBuffer, sizeof(Vec2) * stroker->m_VertexCapacity, 16);
	stroker->m_ColorBuffer = (uint32_t*)BX_ALIGNED_REALLOC(stroker->m_Allocator, stroker->m_ColorBuffer, sizeof(uint32_t) * stroker->m_VertexCapacity, 16);
}

static BX_FORCE_INLINE void expandVB(Stroker* stroker, uint32_t n)
{
	if (stroker->m_NumVertices + n > stroker->m_VertexCapacity) {
		reallocVB(stroker, n);
	}
}

static void reallocIB(Stroker* stroker, uint32_t n)
{
	stroker->m_IndexCapacity += n;
	stroker->m_IndexBuffer = (uint16_t*)BX_ALIGNED_REALLOC(stroker->m_Allocator, stroker->m_IndexBuffer, sizeof(uint16_t) * stroker->m_IndexCapacity, 16);
}

static BX_FORCE_INLINE void expandIB(Stroker* stroker, uint32_t n)
{
	if (stroker->m_NumIndices + n > stroker->m_IndexCapacity) {
		reallocIB(stroker, n);
	}
}

template<uint32_t N>
static void addPos(Stroker* stroker, const Vec2* srcPos)
{
	VG_CHECK(stroker->m_NumVertices + N <= stroker->m_VertexCapacity, "Not enough free space for temporary geometry");

	float* dstPos = &stroker->m_PosBuffer[stroker->m_NumVertices].x;
	memcpy(dstPos, srcPos, sizeof(Vec2) * N);

	stroker->m_NumVertices += N;
}

template<uint32_t N>
static void addPosColor(Stroker* stroker, const Vec2* srcPos, const uint32_t* srcColor)
{
	VG_CHECK(stroker->m_NumVertices + N <= stroker->m_VertexCapacity, "Not enough free space for temporary geometry");

	float* dstPos = &stroker->m_PosBuffer[stroker->m_NumVertices].x;
	memcpy(dstPos, srcPos, sizeof(Vec2) * N);

	uint32_t* dstColor = &stroker->m_ColorBuffer[stroker->m_NumVertices];
	memcpy(dstColor, srcColor, sizeof(uint32_t) * N);

	stroker->m_NumVertices += N;
}

template<uint32_t N>
static void addIndices(Stroker* stroker, const uint16_t* src)
{
	VG_CHECK(stroker->m_NumIndices + N <= stroker->m_IndexCapacity, "Not enough free space for temporary geometry");

	uint16_t* dst = &stroker->m_IndexBuffer[stroker->m_NumIndices];
	memcpy(dst, src, sizeof(uint16_t) * N);

	stroker->m_NumIndices += N;
}
}
