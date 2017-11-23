#include "path.h"
#include "common.h"
#include <bx/allocator.h>
#include <bx/math.h>

namespace vg
{
Path::Path(bx::AllocatorI* allocator) 
	: m_Allocator(allocator)
	, m_Scale(1.0f)
	, m_TesselationTolerance(0.25f)
	, m_Vertices(nullptr)
	, m_NumVertices(0)
	, m_VertexCapacity(0)
	, m_SubPaths(nullptr)
	, m_NumSubPaths(0)
	, m_SubPathCapacity(0)
	, m_CurSubPath(nullptr)
{
}

Path::~Path()
{
	BX_ALIGNED_FREE(m_Allocator, m_Vertices, 16);
	m_Vertices = nullptr;
	m_NumVertices = 0;
	m_VertexCapacity = 0;

	BX_FREE(m_Allocator, m_SubPaths);
	m_SubPaths = nullptr;
	m_NumSubPaths = 0;
	m_SubPathCapacity = 0;
}

void Path::reset(float scale, float tesselationTolerance)
{
	m_Scale = scale;
	m_TesselationTolerance = tesselationTolerance;

	if (m_SubPathCapacity == 0) {
		m_SubPathCapacity = 16;
		m_SubPaths = (SubPath*)BX_ALLOC(m_Allocator, sizeof(SubPath) * m_SubPathCapacity);
	}

	m_NumSubPaths = 0;
	m_NumVertices = 0;
	m_SubPaths[0].m_IsClosed = false;
	m_SubPaths[0].m_NumVertices = 0;
	m_SubPaths[0].m_FirstVertexID = 0;
	m_CurSubPath = nullptr;
}

void Path::moveTo(float x, float y)
{
	if (!m_CurSubPath || m_CurSubPath->m_NumVertices != 0) {
		// Move on to the next sub path.
		if (m_NumSubPaths + 1 > m_SubPathCapacity) {
			m_SubPathCapacity += 16;
			m_SubPaths = (SubPath*)BX_REALLOC(m_Allocator, m_SubPaths, sizeof(SubPath) * m_SubPathCapacity);
		}

		m_CurSubPath = &m_SubPaths[m_NumSubPaths++];
		m_CurSubPath->m_IsClosed = false;
		m_CurSubPath->m_NumVertices = 0;
		m_CurSubPath->m_FirstVertexID = m_NumVertices;
	}

	addVertex(x, y);
}

void Path::lineTo(float x, float y)
{
	BX_CHECK(m_CurSubPath && m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling lineTo()");

	addVertex(x, y);
}

void Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	BX_CHECK(m_CurSubPath && m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling cubicTo()")

	const int MAX_LEVELS = 10;
	static float stack[MAX_LEVELS * 8];

	const uint32_t lastVertexID = m_CurSubPath->m_FirstVertexID + (m_CurSubPath->m_NumVertices - 1);
	const float* lastVertex = &m_Vertices[lastVertexID << 1];

	float x1 = lastVertex[0];
	float y1 = lastVertex[1];
	float x2 = c1x;
	float y2 = c1y;
	float x3 = c2x;
	float y3 = c2y;
	float x4 = x;
	float y4 = y;

	const float tessTol = m_TesselationTolerance / (m_Scale * m_Scale);

	float* stackPtr = stack;
	bool done = false;
	while (!done) {
		const float dx = x4 - x1;
		const float dy = y4 - y1;
		const float d2 = bx::fabs(((x2 - x4) * dy - (y2 - y4) * dx));
		const float d3 = bx::fabs(((x3 - x4) * dy - (y3 - y4) * dx));
		const float d23 = d2 + d3;

		if (d23 * d23 <= tessTol * (dx * dx + dy * dy)) {
			addVertex(x4, y4);

			// Pop sibling off the stack and decrease level...
			if (stackPtr == stack) {
				done = true;
			} else {
				stackPtr -= 8;
				y4 = stackPtr[0];
				x4 = stackPtr[1];
				y3 = stackPtr[2];
				x3 = stackPtr[3];
				y2 = stackPtr[4];
				x2 = stackPtr[5];
				y1 = stackPtr[6];
				x1 = stackPtr[7];
			}
		} else {
			const ptrdiff_t curLevel = (stackPtr - stack); // 8 floats per sub-curve
			if (curLevel < MAX_LEVELS * 8) {
				const float x12 = (x1 + x2) * 0.5f;
				const float y12 = (y1 + y2) * 0.5f;
				const float x23 = (x2 + x3) * 0.5f;
				const float y23 = (y2 + y3) * 0.5f;
				const float x34 = (x3 + x4) * 0.5f;
				const float y34 = (y3 + y4) * 0.5f;
				const float x123 = (x12 + x23) * 0.5f;
				const float y123 = (y12 + y23) * 0.5f;
				const float x234 = (x23 + x34) * 0.5f;
				const float y234 = (y23 + y34) * 0.5f;
				const float x1234 = (x123 + x234) * 0.5f;
				const float y1234 = (y123 + y234) * 0.5f;

				// Push sibling on the stack...
				stackPtr[0] = y4;
				stackPtr[1] = x4;
				stackPtr[2] = y34;
				stackPtr[3] = x34;
				stackPtr[4] = y234;
				stackPtr[5] = x234;
				stackPtr[6] = y1234;
				stackPtr[7] = x1234;
				stackPtr += 8;

//				x1 = x1; // NOP
//				y1 = y1; // NOP
				x2 = x12;
				y2 = y12;
				x3 = x123;
				y3 = y123;
				x4 = x1234;
				y4 = y1234;
			} else {
				// Pop sibling off the stack...
				stackPtr -= 8;
				y4 = stackPtr[0];
				x4 = stackPtr[1];
				y3 = stackPtr[2];
				x3 = stackPtr[3];
				y2 = stackPtr[4];
				x2 = stackPtr[5];
				y1 = stackPtr[6];
				x1 = stackPtr[7];
			}
		}
	}
}

void Path::rect(float x, float y, float w, float h)
{
	if (bx::fabs(w) < VG_EPSILON || bx::fabs(h) < VG_EPSILON) {
		return;
	}

	moveTo(x, y);
	lineTo(x, y + h);
	lineTo(x + w, y + h);
	lineTo(x + w, y);
	close();
}

void Path::roundedRect(float x, float y, float w, float h, float r)
{
	if (r < 0.1f) {
		rect(x, y, w, h);
		return;
	}
	
	const float rx = bx::fmin(r, bx::fabs(w) * 0.5f) * bx::fsign(w);
	const float ry = bx::fmin(r, bx::fabs(h) * 0.5f) * bx::fsign(h);

	r = bx::fmin(rx, ry);

	const float da = bx::facos((m_Scale * r) / ((m_Scale * r) + m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::fceil(bx::kPi / da));
	const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

	const float dtheta = -bx::kPiHalf / (float)(numPointsQuarterCircle - 1);
	const float cos_dtheta = bx::fcos(dtheta);
	const float sin_dtheta = bx::fsin(dtheta);

	moveTo(x, y + r);
	lineTo(x, y + h - r);

	// Bottom left quarter circle
	{
		const float cx = x + r;
		const float cy = y + h - r;
		float* circleVertices = allocVertices(numPointsQuarterCircle - 1);

		float ca = -1.0f; // cosf(-PI);
		float sa = 0.0f;  // sinf(-PI);
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + r * ca;
			circleVertices[1] = cy + r * sa;
			circleVertices += 2;
		}
		m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	lineTo(x + w - r, y + h);

	// Bottom right quarter circle
	{
		const float cx = x + w - r;
		const float cy = y + h - r;
		float* circleVertices = allocVertices(numPointsQuarterCircle - 1);

		float ca = 0.0f; // cosf(-1.5f * PI);
		float sa = 1.0f; // sinf(-1.5f * PI);
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + r * ca;
			circleVertices[1] = cy + r * sa;
			circleVertices += 2;
		}
		m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	lineTo(x + w, y + r);

	// Top right quarter circle
	{
		const float cx = x + w - r;
		const float cy = y + r;
		float* circleVertices = allocVertices(numPointsQuarterCircle - 1);

		float ca = 1.0f; // cosf(0.0f);
		float sa = 0.0f; // sinf(0.0f);
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;
			
			circleVertices[0] = cx + r * ca;
			circleVertices[1] = cy + r * sa;
			circleVertices += 2;
		}
		m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	lineTo(x + r, y);

	// Top left quarter circle
	{
		const float cx = x + r;
		const float cy = y + r;
		float* circleVertices = allocVertices(numPointsQuarterCircle - 1);
		
		float ca = 0.0f; // cosf(-0.5f * PI);
		float sa = -1.0f; // sinf(-0.5f * PI);
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + r * ca;
			circleVertices[1] = cy + r * sa;
			circleVertices += 2;
		}
		m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	close();
}

void Path::circle(float cx, float cy, float r)
{
	const float da = acos((m_Scale * r) / ((m_Scale * r) + m_TesselationTolerance)) * 2.0f;

	const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::fceil(bx::kPi / da));
	const uint32_t numPoints = (numPointsHalfCircle * 2);

	moveTo(cx + r, cy);

	float* circleVertices = allocVertices(numPoints - 1);

	// http://www.iquilezles.org/www/articles/sincos/sincos.htm
	const float dtheta = -bx::kPi2 / (float)numPoints;
	const float cos_dtheta = bx::fcos(dtheta);
	const float sin_dtheta = bx::fsin(dtheta);

	float ca = 1.0f;
	float sa = 0.0f;
	for (uint32_t i = 1; i < numPoints; ++i) {
		const float nextSin = sin_dtheta * ca + cos_dtheta * sa;
		const float nextCos = cos_dtheta * ca - sin_dtheta * sa;
		ca = nextCos;
		sa = nextSin;

		circleVertices[0] = cx + r * ca;
		circleVertices[1] = cy + r * sa;
		circleVertices += 2;
	}

	m_CurSubPath->m_NumVertices += (numPoints - 1);

	close();
}

void Path::polyline(const float* coords, uint32_t numPoints)
{
	BX_CHECK(m_CurSubPath && m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling polyline()");
	BX_CHECK(!m_CurSubPath->m_IsClosed, "Cannot add new vertices to a closed path");

	if (m_CurSubPath->m_NumVertices > 0) {
		const uint32_t lastVertexID = m_CurSubPath->m_FirstVertexID + (m_CurSubPath->m_NumVertices - 1);
		const float* lastVertex = &m_Vertices[lastVertexID << 1];

		const float dx = lastVertex[0] - coords[0];
		const float dy = lastVertex[1] - coords[1];
		const float distSqr = dx * dx + dy * dy;
		if (distSqr < VG_EPSILON) {
			coords += 2;
			numPoints--;
		}
	}

	float* vertices = allocVertices(numPoints);
	bx::memCopy(vertices, coords, sizeof(float) * 2 * numPoints);
	m_CurSubPath->m_NumVertices += numPoints;
}

void Path::close()
{
	BX_CHECK(m_CurSubPath && m_CurSubPath->m_NumVertices != 0, "Cannot close empty path");
	if (m_CurSubPath->m_IsClosed || m_CurSubPath->m_NumVertices <= 2) {
		return;
	}

	m_CurSubPath->m_IsClosed = true;

	const float* firstVertex = &m_Vertices[m_CurSubPath->m_FirstVertexID << 1];
	const float* lastVertex = &m_Vertices[(m_CurSubPath->m_FirstVertexID + (m_CurSubPath->m_NumVertices - 1)) << 1];

	const float dx = lastVertex[0] - firstVertex[0];
	const float dy = lastVertex[1] - firstVertex[1];
	const float distSqr = dx * dx + dy * dy;
	if (distSqr < VG_EPSILON) {
		--m_CurSubPath->m_NumVertices;
		--m_NumVertices;
	}
}

float* Path::allocVertices(uint32_t n)
{
	if (m_NumVertices + n > m_VertexCapacity) {
		m_VertexCapacity = bx::uint32_max(m_VertexCapacity + n, m_VertexCapacity != 0 ? (m_VertexCapacity * 3) >> 1 : 16);
		m_Vertices = (float*)BX_ALIGNED_REALLOC(m_Allocator, m_Vertices, sizeof(float) * 2 * m_VertexCapacity, 16);
	}

	float* p = &m_Vertices[m_NumVertices << 1];
	m_NumVertices += n;

	return p;
}

void Path::addVertex(float x, float y)
{
	// Don't allow adding new vertices to a closed sub-path.
	BX_CHECK(m_CurSubPath, "No path");
	BX_CHECK(!m_CurSubPath->m_IsClosed, "Cannot add new vertices to a closed path");

	if (m_CurSubPath->m_NumVertices != 0) {
		const uint32_t lastVertexID = m_CurSubPath->m_FirstVertexID + (m_CurSubPath->m_NumVertices - 1);
		const float* lastVertex = &m_Vertices[lastVertexID << 1];

		const float dx = lastVertex[0] - x;
		const float dy = lastVertex[1] - y;
		const float distSqr = dx * dx + dy * dy;
		if (distSqr < VG_EPSILON) {
			return;
		}
	}

	float* v = allocVertices(1);
	v[0] = x;
	v[1] = y;

	m_CurSubPath->m_NumVertices++;
}
}
