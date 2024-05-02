#include <vg/path.h>
#include <bx/allocator.h>

namespace vg
{
struct Path
{
	bx::AllocatorI* m_Allocator;
	float* m_Vertices;
	SubPath* m_SubPaths;
	SubPath* m_CurSubPath;
	uint32_t m_NumVertices;
	uint32_t m_VertexCapacity;
	uint32_t m_NumSubPaths;
	uint32_t m_SubPathCapacity;
	float m_Scale;
	float m_TesselationTolerance;
};

static float* pathAllocVertices(Path* path, uint32_t n);
static void pathAddVertex(Path* path, float x, float y);

Path* createPath(bx::AllocatorI* allocator)
{
	Path* path = (Path*)bx::alloc(allocator, sizeof(Path));
	bx::memSet(path, 0, sizeof(Path));
	path->m_Allocator = allocator;
	path->m_Scale = 1.0f;
	path->m_TesselationTolerance = 0.25f;
	return path;
}

void destroyPath(Path* path)
{
	bx::AllocatorI* allocator = path->m_Allocator;

	bx::alignedFree(allocator, path->m_Vertices, 16);
	bx::free(allocator, path->m_SubPaths);
	bx::free(allocator, path);
}

void pathReset(Path* path, float scale, float tesselationTolerance)
{
	path->m_Scale = scale;
	path->m_TesselationTolerance = tesselationTolerance;

	if (path->m_SubPathCapacity == 0) {
		path->m_SubPathCapacity = 16;
		path->m_SubPaths = (SubPath*)bx::alloc(path->m_Allocator, sizeof(SubPath) * path->m_SubPathCapacity);
	}

	path->m_NumSubPaths = 0;
	path->m_NumVertices = 0;
	path->m_SubPaths[0].m_IsClosed = false;
	path->m_SubPaths[0].m_NumVertices = 0;
	path->m_SubPaths[0].m_FirstVertexID = 0;
	path->m_CurSubPath = nullptr;
}

void pathMoveTo(Path* path, float x, float y)
{
	if (!path->m_CurSubPath || path->m_CurSubPath->m_NumVertices != 0) {
		// Move on to the next sub path.
		if (path->m_NumSubPaths + 1 > path->m_SubPathCapacity) {
			path->m_SubPathCapacity += 16;
			path->m_SubPaths = (SubPath*)bx::realloc(path->m_Allocator, path->m_SubPaths, sizeof(SubPath) * path->m_SubPathCapacity);
		}

		path->m_CurSubPath = &path->m_SubPaths[path->m_NumSubPaths++];
		path->m_CurSubPath->m_IsClosed = false;
		path->m_CurSubPath->m_NumVertices = 0;
		path->m_CurSubPath->m_FirstVertexID = path->m_NumVertices;
	}

	pathAddVertex(path, x, y);
}

void pathLineTo(Path* path, float x, float y)
{
	VG_CHECK(path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling lineTo()");
	pathAddVertex(path, x, y);
}

void pathCubicTo(Path* path, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling cubicTo()");

	const int MAX_LEVELS = 10;
	static float stack[MAX_LEVELS * 8];

	const uint32_t lastVertexID = path->m_CurSubPath->m_FirstVertexID + (path->m_CurSubPath->m_NumVertices - 1);
	const float* lastVertex = &path->m_Vertices[lastVertexID << 1];

	float x1 = lastVertex[0];
	float y1 = lastVertex[1];
	float x2 = c1x;
	float y2 = c1y;
	float x3 = c2x;
	float y3 = c2y;
	float x4 = x;
	float y4 = y;

	const float tessTol = path->m_TesselationTolerance / (path->m_Scale * path->m_Scale);

	float* stackPtr = stack;
	bool done = false;
	while (!done) {
		const float dx = x4 - x1;
		const float dy = y4 - y1;
		const float d2 = bx::abs((x2 - x4) * dy - (y2 - y4) * dx);
		const float d3 = bx::abs((x3 - x4) * dy - (y3 - y4) * dx);
		const float d23 = d2 + d3;

		if (d23 * d23 <= tessTol * (dx * dx + dy * dy)) {
			pathAddVertex(path, x4, y4);

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

void pathQuadraticTo(Path* path, float cx, float cy, float x, float y)
{
	// Convert quadratic bezier to cubic bezier (http://fontforge.github.io/bezier.html)
	VG_CHECK(path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling quadraticTo()");

	const uint32_t lastVertexID = path->m_CurSubPath->m_FirstVertexID + (path->m_CurSubPath->m_NumVertices - 1);
	const float* lastVertex = &path->m_Vertices[lastVertexID << 1];

	const float x0 = lastVertex[0];
	const float y0 = lastVertex[1];

	const float c1x = x0 + (2.0f / 3.0f) * (cx - x0);
	const float c1y = y0 + (2.0f / 3.0f) * (cy - y0);
	const float c2x = x + (2.0f / 3.0f) * (cx - x);
	const float c2y = y + (2.0f / 3.0f) * (cy - y);

	pathCubicTo(path, c1x, c1y, c2x, c2y, x, y);
}

void pathArcTo(Path* path, float x1, float y1, float x2, float y2, float r)
{
	VG_CHECK(path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling arcTo()");

	const uint32_t lastVertexID = path->m_CurSubPath->m_FirstVertexID + (path->m_CurSubPath->m_NumVertices - 1);
	const float* lastVertex = &path->m_Vertices[lastVertexID << 1];

	// nvgArcTo()
	const float x0 = lastVertex[0];
	const float y0 = lastVertex[1];

	// TODO: Handle degenerate cases.
//	if (nvg__ptEquals(x0, y0, x1, y1, ctx->distTol) ||
//		nvg__ptEquals(x1, y1, x2, y2, ctx->distTol) ||
//		nvg__distPtSeg(x1, y1, x0, y0, x2, y2) < ctx->distTol * ctx->distTol ||
//		radius < ctx->distTol) 
//	{
//		nvgLineTo(ctx, x1,y1);
//		return;
//	}

	// Calculate tangential circle to lines (x0,y0)-(x1,y1) and (x1,y1)-(x2,y2).
	float dx0 = x0 - x1;
	float dy0 = y0 - y1;
	float dx1 = x2 - x1;
	float dy1 = y2 - y1;
//	nvg__normalize(&dx0, &dy0);
	{
		const float lenSqr = dx0 * dx0 + dy0 * dy0;
		const float invLen = lenSqr < VG_EPSILON ? 0.0f : bx::rsqrt(lenSqr);
		dx0 *= invLen;
		dy0 *= invLen;
	}

//	nvg__normalize(&dx1, &dy1);
	{
		const float lenSqr = dx1 * dx1 + dy1 * dy1;
		const float invLen = lenSqr < VG_EPSILON ? 0.0f : bx::rsqrt(lenSqr);
		dx1 *= invLen;
		dy1 *= invLen;
	}

	const float a = bx::acos(dx0 * dx1 + dy0 * dy1);
	const float d = r / bx::tan(a / 2.0f);

	if (d > 10000.0f) {
		pathLineTo(path, x1, y1);
		return;
	}

	float cx, cy, a0, a1;
	Winding::Enum dir;
	const float cross = dx1 * dy0 - dx0 * dy1;
	if (cross > 0.0f) {
		cx = x1 + dx0 * d + dy0 * r;
		cy = y1 + dy0 * d - dx0 * r;

		a0 = bx::atan2(dx0, -dy0);
		a1 = bx::atan2(-dx1, dy1);
		dir = Winding::CW;
	} else {
		cx = x1 + dx0 * d - dy0 * r;
		cy = y1 + dy0 * d + dx0 * r;

		a0 = bx::atan2(-dx0, dy0);
		a1 = bx::atan2(dx1, -dy1);
		dir = Winding::CCW;
	}

	pathArc(path, cx, cy, r, a0, a1, dir);
}

void pathRect(Path* path, float x, float y, float w, float h)
{
	if (bx::abs(w) < VG_EPSILON || bx::abs(h) < VG_EPSILON) {
		return;
	}

	pathMoveTo(path, x, y);
	pathLineTo(path, x, y + h);
	pathLineTo(path, x + w, y + h);
	pathLineTo(path, x + w, y);
	pathClose(path);
}

void pathRoundedRect(Path* path, float x, float y, float w, float h, float r)
{
	if (r < 0.1f) {
		pathRect(path, x, y, w, h);
		return;
	}

	const float rx = bx::min<float>(r, bx::abs(w) * 0.5f) * bx::sign(w);
	const float ry = bx::min<float>(r, bx::abs(h) * 0.5f) * bx::sign(h);

	r = bx::min<float>(rx, ry);

	const float da = bx::acos((path->m_Scale * r) / ((path->m_Scale * r) + path->m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPi / da));
	const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

	const float dtheta = -bx::kPiHalf / (float)(numPointsQuarterCircle - 1);
	const float cos_dtheta = bx::cos(dtheta);
	const float sin_dtheta = bx::sin(dtheta);

	pathMoveTo(path, x, y + r);
	pathLineTo(path, x, y + h - r);

	// Bottom left quarter circle
	{
		const float cx = x + r;
		const float cy = y + h - r;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

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
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	pathLineTo(path, x + w - r, y + h);

	// Bottom right quarter circle
	{
		const float cx = x + w - r;
		const float cy = y + h - r;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

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
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	pathLineTo(path, x + w, y + r);

	// Top right quarter circle
	{
		const float cx = x + w - r;
		const float cy = y + r;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

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
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	pathLineTo(path, x + r, y);

	// Top left quarter circle
	{
		const float cx = x + r;
		const float cy = y + r;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

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
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	pathClose(path);
}

void pathRoundedRectVarying(Path* path, float x, float y, float w, float h, float rTopLeft, float rTopRight, float rBottomRight, float rBottomLeft)
{
	if (rTopLeft < 0.1f && rBottomLeft < 0.1f && rBottomRight < 0.1f && rTopRight < 0.1f) {
		pathRect(path, x, y, w, h);
		return;
	}

	const float halfw = w * 0.5f;
	const float halfh = h * 0.5f;

	const float rtl = bx::min<float>(rTopLeft, halfw, halfh);
	const float rtr = bx::min<float>(rTopRight, halfw, halfh);
	const float rbl = bx::min<float>(rBottomLeft, halfw, halfh);
	const float rbr = bx::min<float>(rBottomRight, halfw, halfh);

	// Top left corner
	if (rtl < 0.1f) {
		pathMoveTo(path, x, y);
	} else {
		pathMoveTo(path, x + rtl, y);

		const float halfDa = bx::acos((path->m_Scale * rtl) / ((path->m_Scale * rtl) + path->m_TesselationTolerance));
		const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPiHalf / halfDa));
		const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

		const float dtheta = -bx::kPiHalf / (float)(numPointsQuarterCircle - 1);
		const float cos_dtheta = bx::cos(dtheta);
		const float sin_dtheta = bx::sin(dtheta);

		const float cx = x + rtl;
		const float cy = y + rtl;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

		float ca = 0.0f;
		float sa = -1.0f;
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + rtl * ca;
			circleVertices[1] = cy + rtl * sa;
			circleVertices += 2;
		}
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	// Bottom left corner
	if (rbl < 0.1f) {
		pathLineTo(path, x, y + h);
	} else {
		pathLineTo(path, x, y + h - rbl);

		const float halfDa = bx::acos((path->m_Scale * rbl) / ((path->m_Scale * rbl) + path->m_TesselationTolerance));
		const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPiHalf / halfDa));
		const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

		const float dtheta = -bx::kPiHalf / (float)(numPointsQuarterCircle - 1);
		const float cos_dtheta = bx::cos(dtheta);
		const float sin_dtheta = bx::sin(dtheta);

		const float cx = x + rbl;
		const float cy = y + h - rbl;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

		float ca = -1.0f;
		float sa = 0.0f;
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + rbl * ca;
			circleVertices[1] = cy + rbl * sa;
			circleVertices += 2;
		}
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	// Bottom right corner
	if (rbr < 0.1f) {
		pathLineTo(path, x + w, y + h);
	} else {
		pathLineTo(path, x + w - rbr, y + h);

		const float halfDa = bx::acos((path->m_Scale * rbr) / ((path->m_Scale * rbr) + path->m_TesselationTolerance));
		const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPiHalf / halfDa));
		const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

		const float dtheta = -bx::kPiHalf / (float)(numPointsQuarterCircle - 1);
		const float cos_dtheta = bx::cos(dtheta);
		const float sin_dtheta = bx::sin(dtheta);

		const float cx = x + w - rbr;
		const float cy = y + h - rbr;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

		float ca = 0.0f; // cosf(-1.5f * PI);
		float sa = 1.0f; // sinf(-1.5f * PI);
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + rbr * ca;
			circleVertices[1] = cy + rbr * sa;
			circleVertices += 2;
		}
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	// Top right corner
	if (rtr < 0.1f) {
		pathLineTo(path, x + w, y);
	} else {
		pathLineTo(path, x + w, y + rtr);

		const float halfDa = bx::acos((path->m_Scale * rtr) / ((path->m_Scale * rtr) + path->m_TesselationTolerance));
		const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPiHalf / halfDa));
		const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

		const float dtheta = -bx::kPiHalf / (float)(numPointsQuarterCircle - 1);
		const float cos_dtheta = bx::cos(dtheta);
		const float sin_dtheta = bx::sin(dtheta);

		const float cx = x + w - rtr;
		const float cy = y + rtr;
		float* circleVertices = pathAllocVertices(path, numPointsQuarterCircle - 1);

		float ca = 1.0f;
		float sa = 0.0f;
		for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
			const float ns = sin_dtheta * ca + cos_dtheta * sa;
			const float nc = cos_dtheta * ca - sin_dtheta * sa;
			ca = nc;
			sa = ns;

			circleVertices[0] = cx + rtr * ca;
			circleVertices[1] = cy + rtr * sa;
			circleVertices += 2;
		}
		path->m_CurSubPath->m_NumVertices += (numPointsQuarterCircle - 1);
	}

	pathClose(path);
}

void pathCircle(Path* path, float cx, float cy, float r)
{
#if 1
	pathEllipse(path, cx, cy, r, r);
#else
	const float da = bx::acos((path->m_Scale * r) / ((path->m_Scale * r) + path->m_TesselationTolerance)) * 2.0f;

	const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPi / da));
	const uint32_t numPoints = (numPointsHalfCircle * 2);

	pathMoveTo(path, cx + r, cy);

	float* circleVertices = pathAllocVertices(path, numPoints - 1);

	// http://www.iquilezles.org/www/articles/sincos/sincos.htm
	const float dtheta = -bx::kPi2 / (float)numPoints;
	const float cos_dtheta = bx::cos(dtheta);
	const float sin_dtheta = bx::sin(dtheta);

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

	path->m_CurSubPath->m_NumVertices += (numPoints - 1);

	pathClose(path);
#endif
}

void pathEllipse(Path* path, float cx, float cy, float rx, float ry)
{
	const float avgR = (rx + ry) * 0.5f;
	const float da = bx::acos((path->m_Scale * avgR) / ((path->m_Scale * avgR) + path->m_TesselationTolerance)) * 2.0f;

	const uint32_t numPointsHalfCircle = bx::uint32_max(2, (uint32_t)bx::ceil(bx::kPi / da));
	const uint32_t numPoints = (numPointsHalfCircle * 2);

	pathMoveTo(path, cx + rx, cy);

	float* circleVertices = pathAllocVertices(path, numPoints - 1);

	const float dtheta = -bx::kPi2 / (float)numPoints;
	const float cos_dtheta = bx::cos(dtheta);
	const float sin_dtheta = bx::sin(dtheta);

	float ca = 1.0f;
	float sa = 0.0f;
	for (uint32_t i = 1; i < numPoints; ++i) {
		const float nextSin = sin_dtheta * ca + cos_dtheta * sa;
		const float nextCos = cos_dtheta * ca - sin_dtheta * sa;
		ca = nextCos;
		sa = nextSin;

		circleVertices[0] = cx + rx * ca;
		circleVertices[1] = cy + ry * sa;
		circleVertices += 2;
	}

	path->m_CurSubPath->m_NumVertices += (numPoints - 1);

	pathClose(path);
}

void pathArc(Path* path, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	// a0 and a1 are CW angles from the x axis independent of the selected direction of the arc.
	// Make sure a0 is always less than a1 and they are both inside the [0, 2*Pi] circle.
	while (a0 > bx::kPi2) {
		a0 -= bx::kPi2;
	}
	while (a1 > bx::kPi2) {
		a1 -= bx::kPi2;
	}

	if (dir == Winding::CCW) {
		while (a0 < a1) {
			a0 += bx::kPi2;
		}
	} else {
		while (a1 < a0) {
			a1 += bx::kPi2;
		}
	}

	const float da = bx::acos((path->m_Scale * r) / ((path->m_Scale * r) + path->m_TesselationTolerance)) * 2.0f;
	const uint32_t numPoints = bx::uint32_max(2, (uint32_t)bx::ceil(bx::abs(a1 - a0) / da));

	const float dtheta = (a1 - a0) / (float)numPoints;
	const float cos_dtheta = bx::cos(dtheta);
	const float sin_dtheta = bx::sin(dtheta);
	float ca = bx::cos(a0);
	float sa = bx::sin(a0);

	if (path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0) {
		pathLineTo(path, cx + r * ca, cy + r * sa);
	} else {
		pathMoveTo(path, cx + r * ca, cy + r * sa);
	}

	float* circleVertices = pathAllocVertices(path, numPoints);
	for (uint32_t i = 0; i < numPoints; ++i) {
		const float nextSin = sin_dtheta * ca + cos_dtheta * sa;
		const float nextCos = cos_dtheta * ca - sin_dtheta * sa;
		ca = nextCos;
		sa = nextSin;

		circleVertices[0] = cx + r * ca;
		circleVertices[1] = cy + r * sa;
		circleVertices += 2;
	}

	path->m_CurSubPath->m_NumVertices += numPoints;
}

void pathPolyline(Path* path, const float* coords, uint32_t numPoints)
{
	VG_CHECK(path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0, "moveTo() should be called once before calling polyline()");
	VG_CHECK(!path->m_CurSubPath->m_IsClosed, "Cannot add new vertices to a closed path");

	if (path->m_CurSubPath->m_NumVertices > 0) {
		const uint32_t lastVertexID = path->m_CurSubPath->m_FirstVertexID + (path->m_CurSubPath->m_NumVertices - 1);
		const float* lastVertex = &path->m_Vertices[lastVertexID << 1];

		const float dx = lastVertex[0] - coords[0];
		const float dy = lastVertex[1] - coords[1];
		const float distSqr = dx * dx + dy * dy;
		if (distSqr < VG_EPSILON) {
			coords += 2;
			numPoints--;
		}
	}

	float* vertices = pathAllocVertices(path, numPoints);
	bx::memCopy(vertices, coords, sizeof(float) * 2 * numPoints);
	path->m_CurSubPath->m_NumVertices += numPoints;
}

void pathClose(Path* path)
{
	VG_CHECK(path->m_CurSubPath && path->m_CurSubPath->m_NumVertices != 0, "Cannot close empty path");
	if (path->m_CurSubPath->m_IsClosed || path->m_CurSubPath->m_NumVertices <= 2) {
		return;
	}

	path->m_CurSubPath->m_IsClosed = true;

	const float* firstVertex = &path->m_Vertices[path->m_CurSubPath->m_FirstVertexID << 1];
	const float* lastVertex = &path->m_Vertices[(path->m_CurSubPath->m_FirstVertexID + (path->m_CurSubPath->m_NumVertices - 1)) << 1];

	const float dx = lastVertex[0] - firstVertex[0];
	const float dy = lastVertex[1] - firstVertex[1];
	const float distSqr = dx * dx + dy * dy;
	if (distSqr < VG_EPSILON) {
		--path->m_CurSubPath->m_NumVertices;
		--path->m_NumVertices;
	}
}

const float* pathGetVertices(const Path* path)
{
	return path->m_Vertices;
}

uint32_t pathGetNumVertices(const Path* path)
{
	return path->m_NumVertices;
}

const SubPath* pathGetSubPaths(const Path* path)
{
	return path->m_SubPaths;
}

uint32_t pathGetNumSubPaths(const Path* path)
{
	return path->m_NumSubPaths;
}

static float* pathAllocVertices(Path* path, uint32_t n)
{
	if (path->m_NumVertices + n > path->m_VertexCapacity) {
		path->m_VertexCapacity = bx::uint32_max(path->m_VertexCapacity + n, path->m_VertexCapacity != 0 ? (path->m_VertexCapacity * 3) >> 1 : 16);
		path->m_Vertices = (float*)bx::alignedRealloc(path->m_Allocator, path->m_Vertices, sizeof(float) * 2 * path->m_VertexCapacity, 16);
	}

	float* p = &path->m_Vertices[path->m_NumVertices << 1];
	path->m_NumVertices += n;

	return p;
}

static void pathAddVertex(Path* path, float x, float y)
{
	// Don't allow adding new vertices to a closed sub-path.
	VG_CHECK(path->m_CurSubPath, "No path");
	VG_CHECK(!path->m_CurSubPath->m_IsClosed, "Cannot add new vertices to a closed path");

#if 0
	if (path->m_CurSubPath->m_NumVertices != 0) {
		const uint32_t lastVertexID = path->m_CurSubPath->m_FirstVertexID + (path->m_CurSubPath->m_NumVertices - 1);
		const float* lastVertex = &path->m_Vertices[lastVertexID << 1];

		const float dx = lastVertex[0] - x;
		const float dy = lastVertex[1] - y;
		const float distSqr = dx * dx + dy * dy;
		if (distSqr < VG_EPSILON) {
			return;
		}
	}
#endif

	float* v = pathAllocVertices(path, 1);
	v[0] = x;
	v[1] = y;

	path->m_CurSubPath->m_NumVertices++;
}
}
