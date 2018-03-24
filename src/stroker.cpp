#include <vg/stroker.h>
#include "vg_util.h"
#if VG_CONFIG_USE_LIBTESS2
#include "libtess2/tesselator.h"
#endif
#include <bx/allocator.h>
#include <bx/math.h>

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4127) // conditional expression is constant
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4456) // declaration of X hides previous local decleration

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
	Vec2 v = vec2PerpCCW(d01);
	const float cross = vec2Cross(d12, d01);
	if (bx::abs(cross) > VG_EPSILON) {
		v = vec2Scale(vec2Sub(d01, d12), (1.0f / cross));
	}

	return v;
}

#if VG_CONFIG_USE_LIBTESS2
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
#endif

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
#if VG_CONFIG_USE_LIBTESS2
	TESStesselator* m_Tesselator;
	libtess2Allocator m_libTessAllocator;
#else
	uint32_t m_ConcaveScratchCapacity;
	uint8_t* m_ConcaveScratchBuffer;
#endif
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

#if VG_CONFIG_USE_LIBTESS2
	if (stroker->m_Tesselator) {
		tessDeleteTess(stroker->m_Tesselator);
	}

	BX_ALIGNED_FREE(allocator, stroker->m_libTessAllocator.m_Buffer, 16);
#else
	BX_ALIGNED_FREE(allocator, stroker->m_ConcaveScratchBuffer, 16);
#endif

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
	const Vec2* vtx = (const Vec2*)vertexList;

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

	mesh->m_PosBuffer = &vtx[0].x;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = numVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;
}

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

bool strokerConcaveFill(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices)
{
#if VG_CONFIG_USE_LIBTESS2
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

	tessAddContour(stroker->m_Tesselator, 2, vertexList, sizeof(float) * 2, numVertices);

	if (!tessTesselate(stroker->m_Tesselator, TESS_WINDING_POSITIVE, TESS_POLYGONS, 3, 2, nullptr)) {
		return false;
	}

	mesh->m_PosBuffer = tessGetVertices(stroker->m_Tesselator);
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = tessGetElements(stroker->m_Tesselator);
	mesh->m_NumVertices = (uint32_t)tessGetVertexCount(stroker->m_Tesselator);
	mesh->m_NumIndices = (uint32_t)tessGetElementCount(stroker->m_Tesselator) * 3;

	return true;
#else
	// http://www.flipcode.com/archives/Efficient_Polygon_Triangulation.shtml
	if (m_ConcaveScratchCapacity < sizeof(int) * numVertices) {
		m_ConcaveScratchCapacity = sizeof(int) * numVertices;
		m_ConcaveScratchBuffer = (uint8_t*)BX_ALIGNED_REALLOC(m_Allocator, m_ConcaveScratchBuffer, m_ConcaveScratchCapacity, 16);
	}

	int* V = (int*)m_ConcaveScratchBuffer;

	resetGeometry();

	// we want a counter-clockwise polygon in V
	if (calcPolygonArea(vtx, numVertices) > 0.0f) {
		for (uint32_t v = 0; v < numVertices; ++v) {
			V[v] = v;
		}
	} else {
		const uint32_t last = numVertices - 1;
		for (uint32_t v = 0; v < numVertices; ++v) {
			V[v] = last - v;
		}
	}

	int n = (int)numVertices;
	int nv = n;

	// remove nv - 2 Vertices, creating 1 triangle every time
	int count = 2 * nv;   // error detection
	for (int v = nv - 1; nv > 2;) {
		// if we loop, it is probably a non-simple polygon
		if ((count--) <= 0) {
			return false;
		}

		// three consecutive vertices in current polygon, <u,v,w>
		int u = v;
		if (nv <= u) {
			u = 0; // previous
		}

		v = u + 1;
		if (nv <= v) {
			v = 0; // new v
		}

		int w = v + 1;
		if (nv <= w) {
			w = 0; // next
		}

		if (snipTriangle(vtx, numVertices, u, v, w, nv, V)) {
			// true names of the vertices
			uint16_t id[3] = {
				(uint16_t)V[u],
				(uint16_t)V[v],
				(uint16_t)V[w]
			};

			// output Triangle
			expandIB(3);
			addIndices(&id[0], 3);

			// remove v from remaining polygon
			--nv;
			for (int s = v; s < nv; ++s) {
				V[s] = V[s + 1];
			}

			// reset error detection counter
			count = 2 * nv;
		}
	}

	mesh->m_PosBuffer = &vtx[0].x;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = numVertices;
	mesh->m_NumIndices = m_NumIndices;

	return true;
#endif
}

// TODO: Currently AA fringes are placed only on the outside of the original path (expanded by 0.5f * fringeWidth).
// Ideally, the original path should be shrinked by 0.5f * fringeWidth before triangulating it and then expand it
// by fringeWidth.
bool strokerConcaveFillAA(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, uint32_t color)
{
#if VG_CONFIG_USE_LIBTESS2
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

	tessAddContour(stroker->m_Tesselator, 2, vertexList, sizeof(float) * 2, numVertices);

	if (!tessTesselate(stroker->m_Tesselator, TESS_WINDING_POSITIVE, TESS_POLYGONS, 3, 2, nullptr)) {
		return false;
	}

	resetGeometry(stroker);

	const uint32_t numTessVertices = (uint32_t)tessGetVertexCount(stroker->m_Tesselator);
	expandVB(stroker, numTessVertices + numVertices);

	VG_CHECK(numTessVertices >= numVertices, "libtess2 error: Original vertices are missing from the result");

	// 1. Copy libTess2 vertices to internal buffer and make room for AA fringes (1 extra vertex for each original vertex).
	// 2. Set the color of all the internal vertices to 'color'
	{
		// Inlined addPosColor() to memset32 all colors.
		const float* tessVertices = tessGetVertices(stroker->m_Tesselator);
		bx::memCopy(&stroker->m_PosBuffer[0], tessVertices, sizeof(Vec2) * numTessVertices);
		vgutil::memset32(&stroker->m_ColorBuffer[0], numTessVertices, &color);
		stroker->m_NumVertices += numTessVertices;
	}

	// Generate fringe vertices from the original vertex list.
	{
		const Vec2* vtx = (const Vec2*)vertexList;
		const float aa = stroker->m_FringeWidth * 0.5f;
		Vec2 d01 = vec2Dir(vtx[numVertices - 1], vtx[0]);

		Vec2* dstPos = &stroker->m_PosBuffer[numTessVertices];
		for (uint32_t iSegment = 0; iSegment < numVertices; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];
			const Vec2& p2 = vtx[iSegment == numVertices - 1 ? 0 : iSegment + 1];

			const Vec2 d12 = vec2Dir(p1, p2);
			const Vec2 v = calcExtrusionVector(d01, d12);
			const Vec2 v_aa = vec2Scale(v, aa);

			*dstPos++ = vec2Add(p1, v_aa);

			d01 = d12;
		}

		// Copy color with alpha = 0 to all fringe vertices.
		const Color c0 = colorSetAlpha(color, 0);
		vgutil::memset32(&stroker->m_ColorBuffer[numTessVertices], numVertices, &c0);

		stroker->m_NumVertices += numVertices;
	}

	// Generate indices for the fringe quads
	const uint32_t numTessIndices = (uint32_t)tessGetElementCount(stroker->m_Tesselator) * 3;
	expandIB(stroker, numTessIndices + numVertices * 6);
	{
		const uint16_t* tessIndices = tessGetElements(stroker->m_Tesselator);
		bx::memCopy(&stroker->m_IndexBuffer[0], tessIndices, sizeof(uint16_t) * numTessIndices);

		uint16_t* dstIndex = &stroker->m_IndexBuffer[numTessIndices];

		const uint16_t* origToFinalMap = tessGetReverseVertexIndices(stroker->m_Tesselator);
		const uint32_t numSegments = numVertices - 1;
		for (uint32_t iSegment = 0; iSegment < numSegments; ++iSegment) {
			VG_CHECK(origToFinalMap[iSegment] != TESS_UNDEF && origToFinalMap[iSegment + 1] != TESS_UNDEF, "libtess2 error: Original vertex not present in final vertex list!");

			const uint16_t id0 = origToFinalMap[iSegment];
			const uint16_t id1 = origToFinalMap[iSegment + 1];
			const uint16_t id2 = (uint16_t)(numTessVertices + iSegment);
			const uint16_t id3 = (uint16_t)(numTessVertices + iSegment + 1);

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
			const uint16_t id0 = origToFinalMap[numSegments];
			const uint16_t id1 = origToFinalMap[0];
			const uint16_t id2 = (uint16_t)(numTessVertices + numSegments);
			const uint16_t id3 = (uint16_t)(numTessVertices);

			dstIndex[0] = id0;
			dstIndex[1] = id2;
			dstIndex[2] = id1;
			dstIndex[3] = id2;
			dstIndex[4] = id3;
			dstIndex[5] = id1;
		}

		stroker->m_NumIndices += numTessIndices + numVertices * 6;
	}

	mesh->m_PosBuffer = &stroker->m_PosBuffer[0].x;
	mesh->m_ColorBuffer = stroker->m_ColorBuffer;
	mesh->m_IndexBuffer = stroker->m_IndexBuffer;
	mesh->m_NumVertices = stroker->m_NumVertices;
	mesh->m_NumIndices = stroker->m_NumIndices;

	return true;
#else
#error "Not implemented yet"
	// http://www.flipcode.com/archives/Efficient_Polygon_Triangulation.shtml
	if (m_ConcaveScratchCapacity < sizeof(int) * numVertices) {
		m_ConcaveScratchCapacity = sizeof(int) * numVertices;
		m_ConcaveScratchBuffer = (uint8_t*)BX_ALIGNED_REALLOC(m_Allocator, m_ConcaveScratchBuffer, m_ConcaveScratchCapacity, 16);
	}

	int* V = (int*)m_ConcaveScratchBuffer;

	resetGeometry();

	// we want a counter-clockwise polygon in V
	if (calcPolygonArea(vtx, numVertices) > 0.0f) {
		for (uint32_t v = 0; v < numVertices; ++v) {
			V[v] = v;
		}
	} else {
		const uint32_t last = numVertices - 1;
		for (uint32_t v = 0; v < numVertices; ++v) {
			V[v] = last - v;
		}
	}

	int n = (int)numVertices;
	int nv = n;

	// remove nv - 2 Vertices, creating 1 triangle every time
	int count = 2 * nv;   // error detection
	for (int v = nv - 1; nv > 2;) {
		// if we loop, it is probably a non-simple polygon
		if ((count--) <= 0) {
			return false;
		}

		// three consecutive vertices in current polygon, <u,v,w>
		int u = v;
		if (nv <= u) {
			u = 0; // previous
		}

		v = u + 1;
		if (nv <= v) {
			v = 0; // new v
		}

		int w = v + 1;
		if (nv <= w) {
			w = 0; // next
		}

		if (snipTriangle(vtx, numVertices, u, v, w, nv, V)) {
			// true names of the vertices
			uint16_t id[3] = {
				(uint16_t)V[u],
				(uint16_t)V[v],
				(uint16_t)V[w]
			};

			// output Triangle
			expandIB(3);
			addIndices(&id[0], 3);

			// remove v from remaining polygon
			--nv;
			for (int s = v; s < nv; ++s) {
				V[s] = V[s + 1];
			}

			// reset error detection counter
			count = 2 * nv;
		}
	}

	mesh->m_PosBuffer = &vtx[0].x;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = numVertices;
	mesh->m_NumIndices = m_NumIndices;

	return true;
#endif
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

	const uint64_t* srcPos64 = (const uint64_t*)&srcPos->x;
	uint64_t* dstPos64 = (uint64_t*)&stroker->m_PosBuffer[stroker->m_NumVertices].x;

#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
	const __m128* srcPos128 = (const __m128*)&srcPos->x;
	__m128* dstPos128 = (__m128*)&stroker->m_PosBuffer[stroker->m_NumVertices].x;
#endif

	if (N == 1) {
		dstPos64[0] = srcPos64[0]; // x0, y0
	} else if (N == 2) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		dstPos128[0] = srcPos128[0]; // x0, y0, x1, y1
#else
		dstPos64[0] = srcPos64[0]; // x0, y0
		dstPos64[1] = srcPos64[1]; // x1, y1
#endif
	} else {
		bx::memCopy(&stroker->m_PosBuffer[stroker->m_NumVertices], srcPos, sizeof(Vec2) * N);
	}

	stroker->m_NumVertices += N;
}

template<uint32_t N>
static void addPosColor(Stroker* stroker, const Vec2* srcPos, const uint32_t* srcColor)
{
	VG_CHECK(stroker->m_NumVertices + N <= stroker->m_VertexCapacity, "Not enough free space for temporary geometry");

	const uint64_t* srcPos64 = (const uint64_t*)&srcPos->x;
	const uint64_t* srcColor64 = (const uint64_t*)srcColor;
	uint64_t* dstPos64 = (uint64_t*)&stroker->m_PosBuffer[stroker->m_NumVertices].x;
	uint32_t* dstColor = &stroker->m_ColorBuffer[stroker->m_NumVertices];
	uint64_t* dstColor64 = (uint64_t*)&stroker->m_ColorBuffer[stroker->m_NumVertices];

#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
	const __m128* srcPos128 = (const __m128*)&srcPos->x;
	const __m128* srcColor128 = (const __m128*)srcColor;
	__m128* dstPos128 = (__m128*)&stroker->m_PosBuffer[stroker->m_NumVertices].x;
	__m128* dstColor128 = (__m128*)&stroker->m_ColorBuffer[stroker->m_NumVertices];
#endif

	if (N == 2) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		*dstPos128 = *srcPos128; // x0, y0, x1, y1
#else
		dstPos64[0] = srcPos64[0]; // x0, y0
		dstPos64[1] = srcPos64[1]; // x1, y1
#endif

		*dstColor64 = *srcColor64; // c0, c1
	} else if (N == 3) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		*dstPos128 = *srcPos128; // x0, y0, x1, y1
		dstPos64[2] = srcPos64[2]; // x2, y2
#else
		dstPos64[0] = srcPos64[0]; // x0, y0
		dstPos64[1] = srcPos64[1]; // x1, y1
		dstPos64[2] = srcPos64[2]; // x2, y2
#endif

		*dstColor64 = *srcColor64; // c0, c1
		dstColor[2] = srcColor[2]; // c2
	} else if (N == 4) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		dstPos128[0] = srcPos128[0]; // x0, y0, x1, y1
		dstPos128[1] = srcPos128[1]; // x2, y2, x3, y3

		dstColor128[0] = srcColor128[0]; // c0, c1, c2, c3
#else
		dstPos64[0] = srcPos64[0]; // x0, y0
		dstPos64[1] = srcPos64[1]; // x1, y1
		dstPos64[2] = srcPos64[2]; // x2, y2
		dstPos64[3] = srcPos64[3]; // x3, y3

		dstColor64[0] = srcColor64[0]; // c0, c1
		dstColor64[1] = srcColor64[1]; // c2, c3
#endif
	} else {
		bx::memCopy(dstPos64, srcPos, sizeof(Vec2) * N);
		bx::memCopy(dstColor64, srcColor, sizeof(uint32_t) * N);
	}

	stroker->m_NumVertices += N;
}

template<uint32_t N>
static void addIndices(Stroker* stroker, const uint16_t* src)
{
	VG_CHECK(stroker->m_NumIndices + N <= stroker->m_IndexCapacity, "Not enough free space for temporary geometry");

	const uint32_t* src32 = (const uint32_t*)src;
	const uint64_t* src64 = (const uint64_t*)src;
	uint16_t* dst = &stroker->m_IndexBuffer[stroker->m_NumIndices];
	uint32_t* dst32 = (uint32_t*)dst;
	uint64_t* dst64 = (uint64_t*)dst;

#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
	const __m128* src128 = (const __m128*)src;
	__m128* dst128 = (__m128*)dst;
#endif

	if (N == 3) {
		dst32[0] = src32[0]; // 0, 1
		dst[2] = src[2];     // 2
	} else if (N == 6) {
		dst64[0] = src64[0]; // 0, 1, 2, 3
		dst32[2] = src32[2]; // 4, 5
	} else if (N == 9) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		dst128[0] = src128[0]; // 0, 1, 2, 3, 4, 5, 6, 7
		dst[8] = src[8];       // 8
#else
		dst64[0] = src64[0]; // 0, 1, 2, 3
		dst64[1] = src64[1]; // 4, 5, 6, 7
		dst[8] = src[8];     // 8
#endif
	} else if (N == 12) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		dst128[0] = src128[0]; // 0, 1, 2, 3, 4, 5, 6, 7
		dst64[2] = src64[2];   // 8, 9, 10, 11
#else
		dst64[0] = src64[0]; // 0, 1, 2, 3
		dst64[1] = src64[1]; // 4, 5, 6, 7
		dst64[2] = src64[2]; // 8, 9, 10, 11
#endif
	} else if (N == 18) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		dst128[0] = src128[0]; // 0, 1, 2, 3, 4, 5, 6, 7
		dst128[1] = src128[1]; // 8, 9, 10, 11, 12, 13, 14, 15
		dst32[8] = src32[8];   // 16, 17
#else
		dst64[0] = src64[0]; // 0, 1, 2, 3
		dst64[1] = src64[1]; // 4, 5, 6, 7
		dst64[2] = src64[2]; // 8, 9, 10, 11
		dst64[3] = src64[3]; // 12, 13, 14, 15
		dst32[8] = src32[8]; // 16, 17
#endif
	} else if (N == 24) {
#if VG_CONFIG_ENABLE_SIMD && BX_CPU_X86
		dst128[0] = src128[0]; // 0, 1, 2, 3, 4, 5, 6, 7
		dst128[1] = src128[1]; // 8, 9, 10, 11, 12, 13, 14, 15
		dst128[2] = src128[2]; // 16, 17, 18, 19, 20, 21, 22, 23
#else
		dst64[0] = src64[0]; // 0, 1, 2, 3
		dst64[1] = src64[1]; // 4, 5, 6, 7
		dst64[2] = src64[2]; // 8, 9, 10, 11
		dst64[3] = src64[3]; // 12, 13, 14, 15
		dst64[4] = src64[4]; // 16, 17, 18, 19
		dst64[5] = src64[5]; // 20, 21, 22, 22
#endif
	} else {
		bx::memCopy(dst, src, sizeof(uint16_t) * N);
	}

	stroker->m_NumIndices += N;
}

#if !VG_CONFIG_USE_LIBTESS2
float calcPolygonArea(const Vec2* points, uint32_t numPoints)
{
	float A = 0.0f;
	for (uint32_t p = numPoints - 1, q = 0; q < numPoints; p = q++) {
		A += points[p].x * points[q].y - points[q].x * points[p].y;
	}

	return A * 0.5f;
}
bool snipTriangle(const Vec2* points, uint32_t numPoints, int u, int v, int w, int n, const int* V)
{
	BX_UNUSED(numPoints);

	const Vec2& A = points[V[u]];
	const Vec2& B = points[V[v]];
	const Vec2& C = points[V[w]];

	const Vec2 AB = vec2Sub(B, A);
	const Vec2 CA = vec2Sub(A, C);
	const float AB_cross_AC = CA.x * AB.y - AB.x * CA.y;
	if (AB_cross_AC < VG_EPSILON) {
		return false;
	}

	const Vec2 BC = vec2Sub(C, B);
	for (int p = 0; p < n; p++) {
		if ((p == u) || (p == v) || (p == w)) {
			continue;
		}

		const Vec2& P = points[V[p]];

		// Inlined InsideTriangle from original code.
		{
			const Vec2 bp = vec2Sub(P, B);
			const float aCROSSbp = BC.x * bp.y - BC.y * bp.x;
			if (aCROSSbp >= 0.0f) {
				const Vec2 ap = vec2Sub(P, A);
				const float cCROSSap = AB.x * ap.y - AB.y * ap.x;
				if (cCROSSap >= 0.0f) {
					const Vec2 cp = vec2Sub(P, C);
					const float bCROSScp = CA.x * cp.y - CA.y * cp.x;
					if (bCROSScp >= 0.0f) {
						// Point inside triangle
						return false;
					}
				}
			}
		}
	}

	return true;
}
#endif
}
