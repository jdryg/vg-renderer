#include "stroker.h"
#include "util.h"
#if VG_CONFIG_USE_LIBTESS2
#include "libtess2/tesselator.h"
#endif
#include <bx/allocator.h>
#include <bx/math.h>

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4127) // conditional expression is constant

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
	const float invLen = lenSqr < VG_EPSILON ? 0.0f : bx::frsqrt(lenSqr);
	return{ dx * invLen, dy * invLen };
}

inline Vec2 calcExtrusionVector(const Vec2& d01, const Vec2& d12)
{
	// v is the vector from the path point to the outline point, assuming a stroke width of 1.0.
	// Equation obtained by solving the intersection of the 2 line segments. d01 and d12 are 
	// assumed to be normalized.
	Vec2 v = vec2PerpCCW(d01);
	const float cross = vec2Cross(d12, d01);
	if (bx::fabs(cross) > VG_EPSILON) {
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

class StrokerImpl
{
public:
	StrokerImpl(bx::AllocatorI* allocator);
	~StrokerImpl();

	void setParams(float scale, float tesselationTolerance, float fringeWidth);

	template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
	void polylineStroke(Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth);

	template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
	void polylineStrokeAA(Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth, Color color);

	template<LineCap::Enum lineCap, LineJoin::Enum lineJoin>
	void polylineStrokeAAThin(Mesh* mesh, const Vec2* vtx, uint32_t numVertices, Color color, bool closed);

	void convexFill(Mesh* mesh, const Vec2* vtx, uint32_t numVertices);
	void convexFillAA(Mesh* mesh, const Vec2* vtx, uint32_t numVertices, Color color);
	bool concaveFill(Mesh* mesh, const Vec2* vtx, uint32_t numVertices);

private:
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

	void resetGeometry();
	void expandVB(uint32_t n);
	void expandIB(uint32_t n);
	void reallocVB(uint32_t n);
	void reallocIB(uint32_t n);
	void addPos(const Vec2* p, uint32_t n);
	void addPosColor(const Vec2* p, const uint32_t* col, uint32_t n);
	void addIndices(const uint16_t* id, uint32_t n);

#if !VG_CONFIG_USE_LIBTESS2
	float calcPolygonArea(const Vec2* points, uint32_t numPoints);
	bool snipTriangle(const Vec2* points, uint32_t numPoints, int u, int v, int w, int n, const int* V);
#endif
};

Stroker::Stroker(bx::AllocatorI* allocator) 
{
	BX_STATIC_ASSERT(sizeof(StrokerImpl) <= sizeof(m_Internal));
	BX_PLACEMENT_NEW(m_Internal, StrokerImpl)(allocator);
}

Stroker::~Stroker()
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);
	impl->~StrokerImpl();
}

void Stroker::reset(float scale, float tesselationTolerance, float fringeWidth)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);
	impl->setParams(scale, tesselationTolerance, fringeWidth);
}

void Stroker::polylineStroke(Mesh* mesh, const float* vertexList, uint32_t numPathVertices, bool isClosed, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);

	const uint8_t perm = (((uint8_t)lineCap) << 1)
		| (((uint8_t)lineJoin) << 3)
		| (isClosed ? 0x01 : 0x00);

	const Vec2* vtx = (const Vec2*)vertexList;

	switch (perm) {
	case  0: impl->polylineStroke<false, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth);   break;
	case  1: impl->polylineStroke<true, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth);    break;
	case  2: impl->polylineStroke<false, LineCap::Round, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth);  break;
	case  3: impl->polylineStroke<true, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth);    break;
	case  4: impl->polylineStroke<false, LineCap::Square, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth); break;
	case  5: impl->polylineStroke<true, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth);    break;
		// 6 to 7 == invalid line cap type
	case  8: impl->polylineStroke<false, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth);   break;
	case  9: impl->polylineStroke<true, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth);    break;
	case 10: impl->polylineStroke<false, LineCap::Round, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth);  break;
	case 11: impl->polylineStroke<true, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth);    break;
	case 12: impl->polylineStroke<false, LineCap::Square, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth); break;
	case 13: impl->polylineStroke<true, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth);    break;
		// 14 to 15 == invalid line cap type
	case 16: impl->polylineStroke<false, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth);   break;
	case 17: impl->polylineStroke<true, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth);    break;
	case 18: impl->polylineStroke<false, LineCap::Round, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth);  break;
	case 19: impl->polylineStroke<true, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth);    break;
	case 20: impl->polylineStroke<false, LineCap::Square, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth); break;
	case 21: impl->polylineStroke<true, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth);    break;
		// 22 to 32 == invalid line join type
	default:
		VG_WARN(false, "Invalid stroke configuration");
		break;
	}
}

void Stroker::polylineStrokeAA(Mesh* mesh, const float* vertexList, uint32_t numPathVertices, bool isClosed, Color color, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);

	const uint8_t perm = (((uint8_t)lineCap) << 1)
		| (((uint8_t)lineJoin) << 3)
		| (isClosed ? 0x01 : 0x00);

	const Vec2* vtx = (const Vec2*)vertexList;

	switch (perm) {
	case  0: impl->polylineStrokeAA<false, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth, color);   break;
	case  1: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case  2: impl->polylineStrokeAA<false, LineCap::Round, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth, color);  break;
	case  3: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case  4: impl->polylineStrokeAA<false, LineCap::Square, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth, color); break;
	case  5: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
		// 6 to 7 == invalid line cap type
	case  8: impl->polylineStrokeAA<false, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth, color);   break;
	case  9: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 10: impl->polylineStrokeAA<false, LineCap::Round, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth, color);  break;
	case 11: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 12: impl->polylineStrokeAA<false, LineCap::Square, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth, color); break;
	case 13: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Round>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
		// 14 to 15 == invalid line cap type
	case 16: impl->polylineStrokeAA<false, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth, color);   break;
	case 17: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 18: impl->polylineStrokeAA<false, LineCap::Round, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth, color);  break;
	case 19: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
	case 20: impl->polylineStrokeAA<false, LineCap::Square, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth, color); break;
	case 21: impl->polylineStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, strokeWidth, color);    break;
		// 22 to 32 == invalid line join type
	default:
		VG_WARN(false, "Invalid stroke configuration");
		break;
	}
}

void Stroker::polylineStrokeAAThin(Mesh* mesh, const float* vertexList, uint32_t numPathVertices, bool isClosed, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);

	// TODO: Why is isClosed passed as argument instead of template param?
	const uint8_t perm = ((uint8_t)lineCap) | (((uint8_t)lineJoin) << 2);

	const Vec2* vtx = (const Vec2*)vertexList;

	switch (perm) {
	case  0: impl->polylineStrokeAAThin<LineCap::Butt, LineJoin::Miter>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  1: impl->polylineStrokeAAThin<LineCap::Square, LineJoin::Miter>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  2: impl->polylineStrokeAAThin<LineCap::Square, LineJoin::Miter>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  4: impl->polylineStrokeAAThin<LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  5: impl->polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  6: impl->polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  8: impl->polylineStrokeAAThin<LineCap::Butt, LineJoin::Bevel>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case  9: impl->polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(mesh, vtx, numPathVertices, color, isClosed);   break;
	case 10: impl->polylineStrokeAAThin<LineCap::Square, LineJoin::Bevel>(mesh, vtx, numPathVertices, color, isClosed);   break;
	default:
		VG_WARN(false, "Invalid stroke configuration");
		break;
	}
}

void Stroker::convexFill(Mesh* mesh, const float* vertexList, uint32_t numVertices)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);

	const Vec2* vtx = (const Vec2*)vertexList;
	impl->convexFill(mesh, vtx, numVertices);
}

void Stroker::convexFillAA(Mesh* mesh, const float* vertexList, uint32_t numVertices, uint32_t color)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);

	const Vec2* vtx = (const Vec2*)vertexList;
	impl->convexFillAA(mesh, vtx, numVertices, color);
}

bool Stroker::concaveFill(Mesh* mesh, const float* vertexList, uint32_t numVertices)
{
	StrokerImpl* impl = reinterpret_cast<StrokerImpl*>(m_Internal);
	return impl->concaveFill(mesh, (const Vec2*)vertexList, numVertices);
}

//////////////////////////////////////////////////////////////////////////
// StrokerImpl
//
StrokerImpl::StrokerImpl(bx::AllocatorI* allocator)
	: m_Allocator(allocator)
	, m_FringeWidth(1.0f)
	, m_Scale(1.0f)
	, m_TesselationTolerance(0.25f)
	, m_PosBuffer(nullptr)
	, m_ColorBuffer(nullptr)
	, m_IndexBuffer(nullptr)
	, m_NumVertices(0)
	, m_NumIndices(0)
	, m_VertexCapacity(0)
	, m_IndexCapacity(0)
#if VG_CONFIG_USE_LIBTESS2
	, m_Tesselator(nullptr)
#else
	, m_ConcaveScratchBuffer(nullptr)
	, m_ConcaveScratchCapacity(0)
#endif
{
#if VG_CONFIG_USE_LIBTESS2
	bx::memSet(&m_libTessAllocator, 0, sizeof(libtess2Allocator));
#endif
}

StrokerImpl::~StrokerImpl()
{
	BX_ALIGNED_FREE(m_Allocator, m_PosBuffer, 16);
	m_PosBuffer = nullptr;

	BX_ALIGNED_FREE(m_Allocator, m_ColorBuffer, 16);
	m_ColorBuffer = nullptr;

	BX_ALIGNED_FREE(m_Allocator, m_IndexBuffer, 16);
	m_IndexBuffer = nullptr;

	m_NumVertices = 0;
	m_NumIndices = 0;

#if VG_CONFIG_USE_LIBTESS2
	if(m_Tesselator) {
		tessDeleteTess(m_Tesselator);
		m_Tesselator = nullptr;
	}

	BX_ALIGNED_FREE(m_Allocator, m_libTessAllocator.m_Buffer, 16);
	m_libTessAllocator.m_Buffer = nullptr;
	m_libTessAllocator.m_Capacity = 0;
	m_libTessAllocator.m_Size = 0;
#else
	BX_ALIGNED_FREE(m_Allocator, m_ConcaveScratchBuffer, 16);
	m_ConcaveScratchBuffer = nullptr;
	m_ConcaveScratchCapacity = 0;
#endif

	m_Allocator = nullptr;
}

void StrokerImpl::setParams(float scale, float tesselationTolerance, float fringeWidth)
{
	m_Scale = scale;
	m_TesselationTolerance = tesselationTolerance;
	m_FringeWidth = fringeWidth;
}

template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void StrokerImpl::polylineStroke(Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth)
{
	const uint32_t numSegments = numPathVertices - (_Closed ? 0 : 1);
	const float hsw = strokeWidth * 0.5f;
	const float da = bx::facos((m_Scale * hsw) / ((m_Scale * hsw) + m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2u, (uint32_t)bx::fceil(bx::kPi / da));

	resetGeometry();

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

			expandVB(2);
			addPos(&p[0], 2);

			prevSegmentLeftID = 0;
			prevSegmentRightID = 1;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 d01_hsw = vec2Scale(d01, hsw);

			Vec2 p[2] = {
				vec2Add(p0, vec2Sub(l01_hsw, d01_hsw)),
				vec2Sub(p0, vec2Add(l01_hsw, d01_hsw))
			};

			expandVB(2);
			addPos(&p[0], 2);

			prevSegmentLeftID = 0;
			prevSegmentRightID = 1;
		} else if (_LineCap == LineCap::Round) {
			expandVB(numPointsHalfCircle);

			const float startAngle = bx::fatan2(l01.y, l01.x);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle + i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p = { p0.x + ca * hsw, p0.y + sa * hsw };

				addPos(&p, 1);
			}

			expandIB((numPointsHalfCircle - 2) * 3);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				uint16_t id[3] = { 0, (uint16_t)(i + 1), (uint16_t)(i + 2) };
				addIndices(&id[0], 3);
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
				const uint16_t firstVertexID = (uint16_t)m_NumVertices;

				Vec2 p[2] = {
					innerCorner,
					vec2Sub(p1, v_hsw)
				};

				expandVB(2);
				addPos(&p[0], 2);

				if (prevSegmentLeftID != 0xFFFF) {
					VG_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstVertexID + 1), firstVertexID
					};

					expandIB(6);
					addIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstVertexID; // 0
					firstSegmentRightID = firstVertexID + 1; // 1
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
					a01 = bx::fatan2(r01.y, r01.x);
					a12 = bx::fatan2(r12.y, r12.x);
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

				uint16_t firstFanVertexID = (uint16_t)m_NumVertices;
				expandVB(numArcPoints + 2);
				addPos(&p[0], 2);
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;
					float ca = bx::fcos(a);
					float sa = bx::fsin(a);

					Vec2 p = { p1.x + hsw * ca, p1.y + hsw * sa };

					addPos(&p, 1);
				}
				addPos(&p[2], 1);

				if (prevSegmentLeftID != 0xFFFF) {
					VG_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID
					};

					expandIB(6);
					addIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstFanVertexID; // 0
					firstSegmentRightID = firstFanVertexID + 1; // 1
				}

				// Generate the triangle fan.
				expandIB(numArcPoints * 3);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					const uint16_t idBase = firstFanVertexID + (uint16_t)iArcPoint;
					uint16_t id[3] = {
						firstFanVertexID, (uint16_t)(idBase + 1), (uint16_t)(idBase + 2)
					};
					addIndices(&id[0], 3);
				}

				prevSegmentLeftID = firstFanVertexID;
				prevSegmentRightID = firstFanVertexID + (uint16_t)numArcPoints + 1;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 innerCorner = vec2Sub(p1, v_hsw);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)m_NumVertices;

				Vec2 p[2] = {
					innerCorner,
					vec2Add(p1, v_hsw),
				};

				expandVB(2);
				addPos(&p[0], 2);

				if (prevSegmentLeftID != 0xFFFF) {
					VG_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, firstVertexID,
						prevSegmentLeftID, firstVertexID, (uint16_t)(firstVertexID + 1)
					};

					expandIB(6);
					addIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstVertexID + 1; // 1
					firstSegmentRightID = firstVertexID; // 0
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
					a01 = bx::fatan2(l01.y, l01.x);
					a12 = bx::fatan2(l12.y, l12.x);
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

				uint16_t firstFanVertexID = (uint16_t)m_NumVertices;
				expandVB(numArcPoints + 2);
				addPos(&p[0], 2);
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;
					float ca = bx::fcos(a);
					float sa = bx::fsin(a);

					Vec2 p = { p1.x + hsw * ca, p1.y + hsw * sa };
					addPos(&p, 1);
				}
				addPos(&p[2], 1);

				if (prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF) {
					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, firstFanVertexID,
						prevSegmentLeftID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(6);
					addIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstFanVertexID + 1; // 1
					firstSegmentRightID = firstFanVertexID; // 0
				}

				expandIB(numArcPoints * 3);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					const uint16_t idBase = firstFanVertexID + (uint16_t)iArcPoint;
					uint16_t id[3] = {
						firstFanVertexID, (uint16_t)(idBase + 2), (uint16_t)(idBase + 1)
					};
					addIndices(&id[0], 3);
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
			const uint16_t curSegmentLeftID = (uint16_t)m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);

			Vec2 p[2] = {
				vec2Add(p1, l01_hsw),
				vec2Sub(p1, l01_hsw)
			};

			expandVB(2);
			addPos(&p[0], 2);

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + 1), curSegmentLeftID
			};

			expandIB(6);
			addIndices(&id[0], 6);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftID = (uint16_t)m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 d01_hsw = vec2Scale(d01, hsw);

			Vec2 p[2] = {
				vec2Add(p1, vec2Add(l01_hsw, d01_hsw)),
				vec2Sub(p1, vec2Sub(l01_hsw, d01_hsw))
			};

			expandVB(2);
			addPos(&p[0], 2);

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + 1), curSegmentLeftID
			};

			expandIB(6);
			addIndices(&id[0], 6);
		} else if (_LineCap == LineCap::Round) {
			expandVB(numPointsHalfCircle);

			const uint16_t curSegmentLeftID = (uint16_t)m_NumVertices;
			const float startAngle = bx::fatan2(l01.y, l01.x);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle - i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p = { p1.x + ca * hsw, p1.y + sa * hsw };

				addPos(&p, 1);
			}

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1)),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1)), curSegmentLeftID
			};

			expandIB(6 + (numPointsHalfCircle - 2) * 3);
			addIndices(&id[0], 6);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)i;
				uint16_t id[3] = {
					curSegmentLeftID, (uint16_t)(idBase + 2), (uint16_t)(idBase + 1)
				};
				addIndices(&id[0], 3);
			}
		}
	} else {
		// Generate the first segment quad. 
		uint16_t id[6] = {
			prevSegmentLeftID, prevSegmentRightID, firstSegmentRightID,
			prevSegmentLeftID, firstSegmentRightID, firstSegmentLeftID
		};

		expandIB(6);
		addIndices(&id[0], 6);
	}

	mesh->m_PosBuffer = &m_PosBuffer[0].x;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = m_NumVertices;
	mesh->m_NumIndices = m_NumIndices;
}

template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void StrokerImpl::polylineStrokeAA(Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, float strokeWidth, Color color)
{
	const uint32_t numSegments = numPathVertices - (_Closed ? 0 : 1);
	const uint32_t c0 = ColorRGBA::setAlpha(color, 0);
	const uint32_t c0_c_c_c0[4] = { c0, color, color, c0 };
	const float hsw = (strokeWidth - m_FringeWidth) * 0.5f;
	const float hsw_aa = hsw + m_FringeWidth;
	const float da = bx::facos((m_Scale * hsw) / ((m_Scale * hsw) + m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2u, (uint32_t)bx::fceil(bx::kPi / da));

	resetGeometry();

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
			const Vec2 d01_aa = vec2Scale(d01, m_FringeWidth);

			Vec2 p[4] = {
				vec2Add(p0, vec2Sub(l01_hsw_aa, d01_aa)),
				vec2Add(p0, l01_hsw),
				vec2Sub(p0, l01_hsw),
				vec2Sub(p0, vec2Add(l01_hsw_aa, d01_aa))
			};

			expandVB(4);
			addPosColor(&p[0], &c0_c_c_c0[0], 4);

			uint16_t id[6] = {
				0, 2, 1,
				0, 3, 2
			};
			expandIB(6);
			addIndices(&id[0], 6);

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

			expandVB(4);
			addPosColor(&p[0], &c0_c_c_c0[0], 4);

			uint16_t id[6] = {
				0, 2, 1,
				0, 3, 2
			};
			expandIB(6);
			addIndices(&id[0], 6);

			prevSegmentLeftAAID = 0;
			prevSegmentLeftID = 1;
			prevSegmentRightID = 2;
			prevSegmentRightAAID = 3;
		} else if (_LineCap == LineCap::Round) {
			const float startAngle = bx::fatan2(l01.y, l01.x);
			expandVB(numPointsHalfCircle << 1);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle + i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p[2] = {
					{ p0.x + ca * hsw, p0.y + sa * hsw },
					{ p0.x + ca * hsw_aa, p0.y + sa * hsw_aa }
				};

				addPosColor(&p[0], &c0_c_c_c0[2], 2);
			}

			// Generate indices for the triangle fan
			expandIB(numPointsHalfCircle * 9 - 12);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				uint16_t id[3] = {
					0,
					(uint16_t)((i << 1) + 2),
					(uint16_t)((i << 1) + 4)
				};
				addIndices(&id[0], 3);
			}

			// Generate indices for the AA quads
			for (uint32_t i = 0; i < numPointsHalfCircle - 1; ++i) {
				const uint16_t idBase = (uint16_t)(i << 1);
				uint16_t id[6] = {
					idBase, (uint16_t)(idBase + 1), (uint16_t)(idBase + 3),
					idBase, (uint16_t)(idBase + 3), (uint16_t)(idBase + 2)
				};
				addIndices(&id[0], 6);
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
				const uint16_t firstVertexID = (uint16_t)m_NumVertices;

				Vec2 p[4] = {
					innerCornerAA,
					innerCorner,
					vec2Sub(p1, v_hsw),
					vec2Sub(p1, v_hsw_aa)
				};

				expandVB(4);
				addPosColor(&p[0], &c0_c_c_c0[0], 4);

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

					expandIB(18);
					addIndices(&id[0], 18);
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
					a01 = bx::fatan2(r01.y, r01.x);
					a12 = bx::fatan2(r12.y, r12.x);
					if (a12 < a01) {
						a12 += bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a12 - a01) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				const uint16_t firstFanVertexID = (uint16_t)m_NumVertices;
				expandVB(numArcPoints * 2 + 4);

				Vec2 p[2] = {
					innerCornerAA,
					innerCorner
				};
				addPosColor(&p[0], &c0_c_c_c0[0], 2);

				// First arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(r01, hsw)),
						vec2Add(p1, vec2Scale(r01, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(vec2Dot(r01, r12));
						p[0] = vec2Sub(p[0], vec2Scale(d01, (cosAngle * m_FringeWidth)));
					}

					addPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Middle arc vertices
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;

					const Vec2 arcPointDir = { bx::fcos(a), bx::fsin(a) };

					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(arcPointDir, hsw)),
						vec2Add(p1, vec2Scale(arcPointDir, hsw_aa))
					};

					addPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Last arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(r12, hsw)),
						vec2Add(p1, vec2Scale(r12, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(vec2Dot(r01, r12));
						p[0] = vec2Add(p[0], vec2Scale(d12, (cosAngle * m_FringeWidth)));
					}

					addPosColor(&p[0], &c0_c_c_c0[2], 2);
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

					expandIB(18);
					addIndices(&id[0], 18);
				} else {
					VG_CHECK(_Closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstFanVertexID; // 0
					firstSegmentLeftID = firstFanVertexID + 1; // 1
					firstSegmentRightID = firstFanVertexID + 2; // 2
					firstSegmentRightAAID = firstFanVertexID + 3; // 3
				}

				// Generate the slice.
				uint16_t arcID = firstFanVertexID + 2;
				expandIB(numArcPoints * 9);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					uint16_t id[9] = {
						(uint16_t)(firstFanVertexID + 1), arcID, (uint16_t)(arcID + 2),
						arcID, (uint16_t)(arcID + 1), (uint16_t)(arcID + 3),
						arcID, (uint16_t)(arcID + 3), (uint16_t)(arcID + 2)
					};
					addIndices(&id[0], 9);

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
				const uint16_t firstFanVertexID = (uint16_t)m_NumVertices;

				Vec2 p[4] = {
					innerCornerAA,
					innerCorner,
					vec2Add(p1, v_hsw),
					vec2Add(p1, v_hsw_aa)
				};

				expandVB(4);
				addPosColor(&p[0], &c0_c_c_c0[0], 4);

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

					expandIB(18);
					addIndices(&id[0], 18);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 3; // 3
					firstSegmentLeftID = firstFanVertexID + 2; // 2
					firstSegmentRightID = firstFanVertexID + 1; // 1
					firstSegmentRightAAID = firstFanVertexID + 0; // 0
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
					a01 = bx::fatan2(l01.y, l01.x);
					a12 = bx::fatan2(l12.y, l12.x);
					if (a12 > a01) {
						a12 -= bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a01 - a12) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				const uint16_t firstFanVertexID = (uint16_t)m_NumVertices;
				expandVB(numArcPoints * 2 + 4);

				Vec2 p[2] = {
					innerCornerAA,
					innerCorner
				};
				addPosColor(&p[0], &c0_c_c_c0[0], 2);

				// First arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(l01, hsw)),
						vec2Add(p1, vec2Scale(l01, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(vec2Dot(l01, l12));
						p[0] = vec2Sub(p[0], vec2Scale(d01, (cosAngle * m_FringeWidth)));
					}

					addPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Middle arc vertices
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;

					const Vec2 arcPointDir = { bx::fcos(a), bx::fsin(a) };

					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(arcPointDir, hsw)),
						vec2Add(p1, vec2Scale(arcPointDir, hsw_aa))
					};

					addPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Last arc vertex
				{
					Vec2 p[2] = {
						vec2Add(p1, vec2Scale(l12, hsw)),
						vec2Add(p1, vec2Scale(l12, hsw_aa))
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(vec2Dot(l01, l12));
						p[0] = vec2Add(p[0], vec2Scale(d12, (cosAngle * m_FringeWidth)));
					}

					addPosColor(&p[0], &c0_c_c_c0[2], 2);
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

					expandIB(18);
					addIndices(&id[0], 18);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 3; // 3
					firstSegmentLeftID = firstFanVertexID + 2; // 2
					firstSegmentRightID = firstFanVertexID + 1; // 1
					firstSegmentRightAAID = firstFanVertexID + 0; // 0
				}

				// Generate the slice.
				uint16_t arcID = firstFanVertexID + 2;
				expandIB(numArcPoints * 9);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					uint16_t id[9] = {
						(uint16_t)(firstFanVertexID + 1), (uint16_t)(arcID + 2), arcID,
						arcID, (uint16_t)(arcID + 3), (uint16_t)(arcID + 1),
						arcID, (uint16_t)(arcID + 2), (uint16_t)(arcID + 3)
					};
					addIndices(&id[0], 9);

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
			const uint16_t curSegmentLeftAAID = (uint16_t)m_NumVertices;
			const Vec2 l01_hsw = vec2Scale(l01, hsw);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);
			const Vec2 d01_aa = vec2Scale(d01, m_FringeWidth);

			Vec2 p[4] = {
				vec2Add(p1, vec2Add(l01_hsw_aa, d01_aa)),
				vec2Add(p1, l01_hsw),
				vec2Sub(p1, l01_hsw),
				vec2Sub(p1, vec2Sub(l01_hsw_aa, d01_aa))
			};

			expandVB(4);
			addPosColor(&p[0], &c0_c_c_c0[0], 4);

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

			expandIB(24);
			addIndices(&id[0], 24);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftAAID = (uint16_t)m_NumVertices;
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

			expandVB(4);
			addPosColor(&p[0], &c0_c_c_c0[0], 4);

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

			expandIB(24);
			addIndices(&id[0], 24);
		} else if (_LineCap == LineCap::Round) {
			const uint16_t curSegmentLeftID = (uint16_t)m_NumVertices;
			const float startAngle = bx::fatan2(l01.y, l01.x);

			expandVB(numPointsHalfCircle * 2);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle - i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p[2] = {
					{ p1.x + ca * hsw, p1.y + sa * hsw },
					{ p1.x + ca * hsw_aa, p1.y + sa * hsw_aa }
				};

				addPosColor(&p[0], &c0_c_c_c0[2], 2);
			}

			uint16_t id[18] = {
				prevSegmentLeftAAID, prevSegmentLeftID, curSegmentLeftID,
				prevSegmentLeftAAID, curSegmentLeftID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2), curSegmentLeftID,
				prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2 + 1),
				prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2 + 1), (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2)
			};

			expandIB(18);
			addIndices(&id[0], 18);

			// Generate indices for the triangle fan
			expandIB((numPointsHalfCircle - 2) * 3);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)(i << 1);
				uint16_t id[3] = {
					curSegmentLeftID,
					(uint16_t)(idBase + 4),
					(uint16_t)(idBase + 2)
				};
				addIndices(&id[0], 3);
			}

			// Generate indices for the AA quads
			expandIB((numPointsHalfCircle - 1) * 6);
			for (uint32_t i = 0; i < numPointsHalfCircle - 1; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)(i << 1);
				uint16_t id[6] = {
					idBase, (uint16_t)(idBase + 3), (uint16_t)(idBase + 1),
					idBase, (uint16_t)(idBase + 2), (uint16_t)(idBase + 3)
				};
				addIndices(&id[0], 6);
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

		expandIB(18);
		addIndices(&id[0], 18);
	}

	mesh->m_PosBuffer = &m_PosBuffer[0].x;
	mesh->m_ColorBuffer = m_ColorBuffer;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = m_NumVertices;
	mesh->m_NumIndices = m_NumIndices;
}

template<LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void StrokerImpl::polylineStrokeAAThin(Mesh* mesh, const Vec2* vtx, uint32_t numPathVertices, Color color, bool closed)
{
	const uint32_t numSegments = numPathVertices - (closed ? 0 : 1);
	const uint32_t c0 = ColorRGBA::setAlpha(color, 0);
	const uint32_t c0_c_c0_c0[4] = { c0, color, c0, c0 };
	const float hsw_aa = m_FringeWidth;

	resetGeometry();

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

			expandVB(3);
			addPosColor(&p[0], &c0_c_c0_c0[0], 3);

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

			expandVB(3);
			addPosColor(&p[0], &c0_c_c0_c0[0], 3);

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
				const uint16_t firstVertexID = (uint16_t)m_NumVertices;

				Vec2 p[3] = {
					innerCorner,
					p1,
					vec2Sub(p1, v_hsw_aa)
				};

				expandVB(3);
				addPosColor(&p[0], &c0_c_c0_c0[0], 3);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstVertexID + 1), firstVertexID,
						prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(firstVertexID + 2),
						prevSegmentMiddleID, (uint16_t)(firstVertexID + 2), (uint16_t)(firstVertexID + 1)
					};

					expandIB(12);
					addIndices(&id[0], 12);
				} else {
					VG_CHECK(closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstVertexID; // 0
					firstSegmentMiddleID = firstVertexID + 1; // 1
					firstSegmentRightAAID = firstVertexID + 2; // 3
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

				const uint16_t firstFanVertexID = (uint16_t)m_NumVertices;
				expandVB(4);
				addPosColor(&p[0], &c0_c_c0_c0[0], 4);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID,
						prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(12);
					addIndices(&id[0], 12);
				} else {
					VG_CHECK(closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstFanVertexID;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 2;
				}

				uint16_t id[3] = {
					(uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3)
				};
				expandIB(3);
				addIndices(&id[0], 3);

				prevSegmentLeftAAID = firstFanVertexID;
				prevSegmentMiddleID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID + 3;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 innerCorner = vec2Sub(p1, v_hsw_aa);

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstFanVertexID = (uint16_t)m_NumVertices;

				Vec2 p[3] = {
					innerCorner,
					p1,
					vec2Add(p1, v_hsw_aa)
				};

				expandVB(3);
				addPosColor(&p[0], &c0_c_c0_c0[0], 3);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentMiddleID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(12);
					addIndices(&id[0], 12);
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

				const uint16_t firstFanVertexID = (uint16_t)m_NumVertices;
				expandVB(4);
				addPosColor(&p[0], &c0_c_c0_c0[0], 4);

				if (prevSegmentLeftAAID != 0xFFFF) {
					VG_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentMiddleID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					expandIB(12);
					addIndices(&id[0], 12);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 2;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				uint16_t id[3] = {
					(uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 3), (uint16_t)(firstFanVertexID + 2)
				};
				addIndices(&id[0], 3);

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
			const uint16_t curSegmentLeftAAID = (uint16_t)m_NumVertices;
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);

			Vec2 p[3] = {
				vec2Add(p1, l01_hsw_aa),
				p1,
				vec2Sub(p1, l01_hsw_aa)
			};

			expandVB(3);
			addPosColor(&p[0], &c0_c_c0_c0[0], 3);

			uint16_t id[12] = {
				prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1)
			};

			expandIB(12);
			addIndices(&id[0], 12);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftAAID = (uint16_t)m_NumVertices;
			const Vec2 d01_hsw = vec2Scale(d01, hsw_aa);
			const Vec2 l01_hsw_aa = vec2Scale(l01, hsw_aa);

			Vec2 p[3] = {
				vec2Add(p1, vec2Add(l01_hsw_aa, d01_hsw)),
				p1,
				vec2Sub(p1, vec2Sub(l01_hsw_aa, d01_hsw))
			};

			expandVB(3);
			addPosColor(&p[0], &c0_c_c0_c0[0], 3);

			uint16_t id[12] = {
				prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1)
			};

			expandIB(12);
			addIndices(&id[0], 12);
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

		expandIB(12);
		addIndices(&id[0], 12);
	}

	mesh->m_PosBuffer = &m_PosBuffer[0].x;
	mesh->m_ColorBuffer = m_ColorBuffer;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = m_NumVertices;
	mesh->m_NumIndices = m_NumIndices;
}

void StrokerImpl::convexFill(Mesh* mesh, const Vec2* vtx, uint32_t numVertices)
{
	BX_UNUSED(vtx);

	const uint32_t numTris = numVertices - 2;
	const uint32_t numIndices = numTris * 3; // N - 2 triangles in a N pt fan, 3 indices per triangle

	resetGeometry();

	// Index Buffer
	{
		expandIB(numIndices);

		uint16_t* dstIndex = m_IndexBuffer;
		uint16_t nextID = 1;

		uint32_t n = numTris;
		while (n-- > 0) {
			*dstIndex++ = 0;
			*dstIndex++ = nextID;
			*dstIndex++ = nextID + 1;

			++nextID;
		}

		m_NumIndices += numIndices;
	}

	mesh->m_PosBuffer = &vtx[0].x;
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = numVertices;
	mesh->m_NumIndices = m_NumIndices;
}

void StrokerImpl::convexFillAA(Mesh* mesh, const Vec2* vtx, uint32_t numVertices, Color color)
{
	const float aa = m_FringeWidth * 0.5f;
	const uint32_t c0 = ColorRGBA::setAlpha(color, 0);

	const uint32_t numTris =
		(numVertices - 2) + // Triangle fan
		(numVertices * 2); // AA fringes
	const uint32_t numDrawVertices = numVertices * 2; // original polygon point + AA fringe point.
	const uint32_t numDrawIndices = numTris * 3;

	resetGeometry();

	// Vertex buffer
	{
		// TODO: Assumes CCW order (otherwise fringes are generated on the wrong side of the polygon)
		expandVB(numDrawVertices);

		Vec2 d01 = vec2Dir(vtx[numVertices - 1], vtx[0]);

		Vec2* dstPos = m_PosBuffer;
		for (uint32_t iSegment = 0; iSegment < numVertices; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];
			const Vec2& p2 = vtx[iSegment == numVertices - 1 ? 0 : iSegment + 1];

			const Vec2 d12 = vec2Dir(p1, p2);
			const Vec2 v = calcExtrusionVector(d01, d12);
			const Vec2 v_aa = vec2Scale(v, aa);

			dstPos[0] = vec2Sub(p1, v_aa);
			dstPos[1] = vec2Add(p1, v_aa);
			dstPos += 2;

			d01 = d12;
		}

		const uint32_t colors[2] = { color, c0 };
		memset64(m_ColorBuffer, numVertices, &colors[0]);

		m_NumVertices += numDrawVertices;
	}

	// Index buffer
	{
		expandIB(numDrawIndices);

		uint16_t* dstIndex = m_IndexBuffer;

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

		m_NumIndices += numDrawIndices;
	}

	mesh->m_PosBuffer = &m_PosBuffer[0].x;
	mesh->m_ColorBuffer = m_ColorBuffer;
	mesh->m_IndexBuffer = m_IndexBuffer;
	mesh->m_NumVertices = m_NumVertices;
	mesh->m_NumIndices = m_NumIndices;
}

bool StrokerImpl::concaveFill(Mesh* mesh, const Vec2* vtx, uint32_t numVertices)
{
#if VG_CONFIG_USE_LIBTESS2
	// Delete old tesselator
	if (m_Tesselator) {
		tessDeleteTess(m_Tesselator);
	}

#if VG_CONFIG_LIBTESS2_SCRATCH_BUFFER
	// Initialize the allocator once
	if (!m_libTessAllocator.m_Buffer) {
		m_libTessAllocator.m_Capacity = VG_CONFIG_LIBTESS2_SCRATCH_BUFFER;
		m_libTessAllocator.m_Buffer = (uint8_t*)BX_ALIGNED_ALLOC(m_Allocator, m_libTessAllocator.m_Capacity, 16);
	}

	// Reset the allocator.
	m_libTessAllocator.m_Size = 0;

	// Initialize the tesselator
	TESSalloc allocator;
	allocator.meshEdgeBucketSize = 16;
	allocator.meshVertexBucketSize = 16;
	allocator.meshFaceBucketSize = 16;
	allocator.dictNodeBucketSize = 16;
	allocator.regionBucketSize = 16;
	allocator.extraVertices = 256;
	allocator.userData = &m_libTessAllocator;
	allocator.memalloc = libtess2Alloc;
	allocator.memfree = libtess2Free;
	allocator.memrealloc = nullptr;
	m_Tesselator = tessNewTess(&allocator);
#else
	m_Tesselator = tessNewTess(nullptr);
#endif

	tessAddContour(m_Tesselator, 2, &vtx[0].x, sizeof(float) * 2, numVertices);

	if (!tessTesselate(m_Tesselator, TESS_WINDING_POSITIVE, TESS_POLYGONS, 3, 2, nullptr)) {
		return false;
	}

	mesh->m_PosBuffer = tessGetVertices(m_Tesselator);
	mesh->m_ColorBuffer = nullptr;
	mesh->m_IndexBuffer = tessGetElements(m_Tesselator);
	mesh->m_NumVertices = (uint32_t)tessGetVertexCount(m_Tesselator);
	mesh->m_NumIndices = (uint32_t)tessGetElementCount(m_Tesselator) * 3;

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

//////////////////////////////////////////////////////////////////////////
// Helpers
//
inline void StrokerImpl::resetGeometry()
{
	m_NumVertices = 0;
	m_NumIndices = 0;
}

BX_FORCE_INLINE void StrokerImpl::expandVB(uint32_t n)
{
	if (m_NumVertices + n > m_VertexCapacity) {
		reallocVB(n);
	}
}

void StrokerImpl::reallocVB(uint32_t n)
{
	m_VertexCapacity += n;
	m_PosBuffer = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_PosBuffer, sizeof(Vec2) * m_VertexCapacity, 16);
	m_ColorBuffer = (uint32_t*)BX_ALIGNED_REALLOC(m_Allocator, m_ColorBuffer, sizeof(uint32_t) * m_VertexCapacity, 16);
}

BX_FORCE_INLINE void StrokerImpl::expandIB(uint32_t n)
{
	if (m_NumIndices + n > m_IndexCapacity) {
		reallocIB(n);
	}
}

void StrokerImpl::reallocIB(uint32_t n)
{
	m_IndexCapacity += n;
	m_IndexBuffer = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, m_IndexBuffer, sizeof(uint16_t) * m_IndexCapacity, 16);
}

BX_FORCE_INLINE void StrokerImpl::addPos(const Vec2* srcPos, uint32_t n)
{
	VG_CHECK(m_NumVertices + n <= m_VertexCapacity, "Not enough free space for temporary geometry");

	bx::memCopy(&m_PosBuffer[m_NumVertices], srcPos, sizeof(Vec2) * n);

	m_NumVertices += n;
}

BX_FORCE_INLINE void StrokerImpl::addPosColor(const Vec2* srcPos, const uint32_t* srcColor, uint32_t n)
{
	VG_CHECK(m_NumVertices + n <= m_VertexCapacity, "Not enough free space for temporary geometry");

	bx::memCopy(&m_PosBuffer[m_NumVertices], srcPos, sizeof(Vec2) * n);
	bx::memCopy(&m_ColorBuffer[m_NumVertices], srcColor, sizeof(uint32_t) * n);

	m_NumVertices += n;
}

BX_FORCE_INLINE void StrokerImpl::addIndices(const uint16_t* src, uint32_t n)
{
	VG_CHECK(m_NumIndices + n <= m_IndexCapacity, "Not enough free space for temporary geometry");

	bx::memCopy(&m_IndexBuffer[m_NumIndices], src, sizeof(uint16_t) * n);

	m_NumIndices += n;
}

#if !VG_CONFIG_USE_LIBTESS2
float StrokerImpl::calcPolygonArea(const Vec2* points, uint32_t numPoints)
{
	float A = 0.0f;
	for (uint32_t p = numPoints - 1, q = 0; q < numPoints; p = q++) {
		A += points[p].x * points[q].y - points[q].x * points[p].y;
	}

	return A * 0.5f;
}
bool StrokerImpl::snipTriangle(const Vec2* points, uint32_t numPoints, int u, int v, int w, int n, const int* V)
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
