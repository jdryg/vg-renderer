#ifndef VG_PATH_H
#define VG_PATH_H

#include <stdint.h>
#include "vg.h"

namespace vg
{
struct Path;

struct SubPath
{
	uint32_t m_FirstVertexID;
	uint32_t m_NumVertices;
	bool m_IsClosed;
};

// Path
Path* createPath(bx::AllocatorI* allocator);
void destroyPath(Path* path);
void pathReset(Path* path, float scale, float tesselationTolerance);
void pathMoveTo(Path* path, float x, float y);
void pathLineTo(Path* path, float x, float y);
void pathCubicTo(Path* path, float c1x, float c1y, float c2x, float c2y, float x, float y);
void pathQuadraticTo(Path* path, float cx, float cy, float x, float y);
void pathArcTo(Path* path, float x1, float y1, float x2, float y2, float r);
void pathRect(Path* path, float x, float y, float w, float h);
void pathRoundedRect(Path* path, float x, float y, float w, float h, float r);
void pathRoundedRectVarying(Path* path, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
void pathCircle(Path* path, float x, float y, float r);
void pathEllipse(Path* path, float x, float y, float rx, float ry);
void pathArc(Path* path, float x, float y, float r, float a0, float a1, Winding::Enum dir);
void pathPolyline(Path* path, const float* coords, uint32_t numPoints);
void pathClose(Path* path);
const float* pathGetVertices(const Path* path);
uint32_t pathGetNumVertices(const Path* path);
const SubPath* pathGetSubPaths(const Path* path);
uint32_t pathGetNumSubPaths(const Path* path);
}

#endif
