#ifndef VG_STROKER_H
#define VG_STROKER_H

#include <stdint.h>
#include "vg.h"

namespace vg
{
struct Stroker;

Stroker* createStroker(bx::AllocatorI* allocator);
void destroyStroker(Stroker* stroker);

void strokerReset(Stroker* stroker, float scale, float tesselationTolerance, float fringeWidth);

/* Geometry
* @----------------------------------@
* |                                  |
* |                                  |
* a                                  b
* |                                  |
* |                                  |
* @----------------------------------@
*
* Generates positions and indices. All vertices have the same color.
* Polyline vertices (a, b) are not part of the geometry.
*/
void strokerPolylineStroke(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, bool isClosed, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin);

/* Geometry
* #----------------------------------#
* |             AA Fringe            |
* @----------------------------------@
* |                                  |
* |                                  |
* a                                  b
* |                                  |
* |                                  |
* @----------------------------------@
* |             AA Fringe            |
* #----------------------------------#
*
* Generates positions, colors and indices.
* Outer vertices (#) have alpha = 0 and inner vertices (@) have alpha = initial value.
* Polyline vertices (a, b) are not part of the geometry.
*/
void strokerPolylineStrokeAA(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, bool isClosed, Color color, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin);

/* Geometry
* #----------------------------------#
* |             AA Fringe            |
* a----------------------------------b
* |             AA Fringe            |
* #----------------------------------#
*
* Generates positions, colors and indices.
* Outer vertices (#) have alpha = 0 and inner vertices (a,b) have alpha = initial value.
*/
void strokerPolylineStrokeAAThin(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, bool isClosed, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin);

/*
* Generates only indices (a triangle fan).
* Positions are the initial polygon vertices (the same pointer is returned in the mesh).
* All vertices have the same color.
*/
void strokerConvexFill(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices);

/*
* Generates positions, colors and indices
*/
void strokerConvexFillAA(Stroker* stroker, Mesh* mesh, const float* vertexList, uint32_t numVertices, uint32_t color);

bool strokerConcaveFillBegin(Stroker* stroker);
void strokerConcaveFillAddContour(Stroker* stroker, const float* vertexList, uint32_t numVertices);

/*
* Generates positions and indices.
* All vertices have the same color.
*/
bool strokerConcaveFillEnd(Stroker* stroker, Mesh* mesh, FillRule::Enum fillRule);

/*
* Generates positions, colors and indices
*/
bool strokerConcaveFillEndAA(Stroker* stroker, Mesh* mesh, uint32_t color, FillRule::Enum fillRule);
}

#endif
