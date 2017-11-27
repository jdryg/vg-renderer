#ifndef VG_STROKER_H
#define VG_STROKER_H

#include <stdint.h>
#include "common.h"
#include <bx/bx.h>

namespace bx
{
struct AllocatorI;
}

namespace vg
{
class StrokerImpl;

class Stroker
{
public:
	Stroker(bx::AllocatorI* allocator);
	~Stroker();

	void reset(float scale, float tesselationTolerance, float fringeWidth);

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
	void polylineStroke(Mesh* mesh, const float* vertexList, uint32_t numVertices, bool isClosed, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin);

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
	void polylineStrokeAA(Mesh* mesh, const float* vertexList, uint32_t numVertices, bool isClosed, Color color, float strokeWidth, LineCap::Enum lineCap, LineJoin::Enum lineJoin);

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
	void polylineStrokeAAThin(Mesh* mesh, const float* vertexList, uint32_t numVertices, bool isClosed, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin);

	/* 
	 * Generates only indices (a triangle fan). 
	 * Positions are the initial polygon vertices (the same pointer is returned in the mesh). 
	 * All vertices have the same color.
	 */
	void convexFill(Mesh* mesh, const float* vertexList, uint32_t numVertices);

	/*
	 * Generates positions, colors and indices
	 */
	void convexFillAA(Mesh* mesh, const float* vertexList, uint32_t numVertices, uint32_t color);

	/*
	 * Generates positions and indices.
	 * All vertices have the same color.
	 */
	bool concaveFill(Mesh* mesh, const float* vertexList, uint32_t numVertices);

private:
	BX_ALIGN_DECL(16, uint8_t) m_Internal[128];
};
}

#endif
