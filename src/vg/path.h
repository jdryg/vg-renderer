#ifndef	VG_PATH_H
#define VG_PATH_H

#include <stdint.h>

namespace bx
{
struct AllocatorI;
}

namespace vg
{
struct SubPath
{
	uint32_t m_FirstVertexID;
	uint32_t m_NumVertices;
	bool m_IsClosed;
};

class Path
{
public:
	Path(bx::AllocatorI* allocator);
	~Path();

	void reset(float scale, float tesselationTolerance);
	void moveTo(float x, float y);
	void lineTo(float x, float y);
	void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	void rect(float x, float y, float w, float h);
	void roundedRect(float x, float y, float w, float h, float r);
	void roundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
	void circle(float x, float y, float r);
	void polyline(const float* coords, uint32_t numPoints);
	void close();

	const float* getVertices() const;
	uint32_t getNumVertices() const;
	const SubPath* getSubPaths() const;
	uint32_t getNumSubPaths() const;

private:
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

	float* allocVertices(uint32_t n);
	void addVertex(float x, float y);
};

//////////////////////////////////////////////////////////////////////////
// Inline functions...
inline const float* Path::getVertices() const
{
	return m_Vertices;
}

inline uint32_t Path::getNumVertices() const
{
	return m_NumVertices;
}

inline const SubPath* Path::getSubPaths() const
{
	return m_SubPaths;
}

inline uint32_t Path::getNumSubPaths() const
{
	return m_NumSubPaths;
}
}

#endif
