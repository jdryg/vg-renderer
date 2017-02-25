// TODO:
// 1) LineJoin::Bevel
// 2) LineJoin::Round
// 3) LineCap::Round
// 4) Layers
// 5) Check polygon winding and force CCW order because otherwise AA fringes are generated inside the polygon!
// 6) Preprocess path vertices before emitting any triangles in Stroke/Fill to avoid calculating directions/normals multiple times in case
//    a path is both Stroke'd and Fill'ed. Problem is that concave polygon decomposition won't work correctly. 

#pragma warning(disable: 4127) // conditional expression is constant (e.g. BezierTo)
#pragma warning(disable: 4706) // assignment withing conditional expression

#include "bgfxvg_renderer.h"

//#define FONTSTASH_IMPLEMENTATION 
#include "../nanovg/fontstash.h"

#include <bgfx/embedded_shader.h>
#include <bx/allocator.h>
#include <bx/fpumath.h>
#include <bx/mutex.h>
#include <bx/simd_t.h>
#include <bx/handlealloc.h>
#include <memory.h>
#include <math.h>
#include <float.h>
#include <assert.h>

// Shaders
#include "vs_solid_color.bin.h"
#include "fs_solid_color.bin.h"
#include "vs_gradient.bin.h"
#include "fs_gradient.bin.h"

#if 1
#include "../util/math.h"
namespace vg
{
using namespace Util;
}
#endif

namespace vg
{
#define MAX_STATE_STACK_SIZE     16
#define MAX_VB_VERTICES          65536
#define MAX_GRADIENTS            64
#define MAX_IMAGE_PATTERNS       64
#define MAX_TEXTURES             64
#define MAX_FONT_IMAGES          4
#define MAX_FONTS                8

#define BATCH_TRANSFORM          1
#define BATCH_PATH_DIRECTIONS    0
#define APPROXIMATE_MATH         1
#define BEZIER_CIRCLE            0

#if BEZIER_CIRCLE
// See: http://spencermortensen.com/articles/bezier-circle/
// TODO: Check if the better approximation gives better results (better == same or better quality with less vertices).
#define NVG_KAPPA90              0.5522847493f // Length proportional to radius of a cubic bezier handle for 90deg arcs.
#endif

static const bgfx::EmbeddedShader s_EmbeddedShaders[] =
{
	BGFX_EMBEDDED_SHADER(vs_solid_color),
	BGFX_EMBEDDED_SHADER(fs_solid_color),
	BGFX_EMBEDDED_SHADER(vs_gradient),
	BGFX_EMBEDDED_SHADER(fs_gradient),

	BGFX_EMBEDDED_SHADER_END()
};

struct State
{
	float m_TransformMtx[6];
	float m_ScissorRect[4];
	float m_GlobalAlpha;
	float m_FontScale;
	float m_AvgScale;
};

struct SubPath
{
	uint32_t m_FirstVertexID;
	uint32_t m_NumVertices;
	bool m_IsClosed;
};

// 20 bytes / vertex * 65536 vertices / buffer = 1.25MB / VB
struct DrawVertex
{
	float x, y;
	float u, v;
	uint32_t color;
};

#define SET_DRAW_VERTEX(dvPtr, px, py, s, t, c) \
	dvPtr->x = (px); \
	dvPtr->y = (py); \
	dvPtr->u = (s); \
	dvPtr->v = (t); \
	dvPtr->color = (c);

struct VertexBuffer
{
	DrawVertex* m_Vertices;
	uint32_t m_Count;
	bgfx::DynamicVertexBufferHandle m_bgfxHandle;
};

struct IndexBuffer
{
	uint16_t* m_Indices;
	uint32_t m_Count;
	uint32_t m_Capacity;

	IndexBuffer() : m_Indices(nullptr), m_Count(0), m_Capacity(0)
	{}

	void reset()
	{
		m_Count = 0;
	}
};

struct ImagePattern
{
	float m_Matrix[6];
	float m_Alpha;
	ImageHandle m_ImageHandle;
};

struct Gradient
{
	float m_Matrix[9];
	float m_Params[4]; // {Extent.x, Extent.y, Radius, Feather}
	float m_InnerColor[4];
	float m_OuterColor[4];
};

// All textures are RGBA
struct Image
{
	bgfx::TextureHandle m_bgfxHandle;
	uint16_t m_Width;
	uint16_t m_Height;
	uint32_t m_Flags;

	inline void reset()
	{
		m_bgfxHandle = BGFX_INVALID_HANDLE;
		m_Width = 0;
		m_Height = 0;
		m_Flags = 0;
	}
};

struct DrawCommand
{
	enum Type : uint32_t
	{
		Type_TexturedVertexColor = 0,
		Type_ColorGradient = 1,
		NumTypes
	};

	uint32_t m_VertexBufferID;
	IndexBuffer* m_IB; // TODO: Index buffer is common to all draw calls at the moment. Either remove the ptr (and get it from the Context) or somehow break commands into different IBs
	GradientHandle m_GradientHandle; // Type_ColorGradient
	ImageHandle m_ImageHandle; // Type_TexturedVertexColor
	uint32_t m_FirstVertexID;
	uint32_t m_FirstIndexID;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
	float m_ScissorRect[4];
	Type m_Type;
};

struct Context
{
	bx::AllocatorI* m_Allocator;

	VertexBuffer* m_VertexBuffers;
	uint32_t m_NumVertexBuffers;
	uint32_t m_VertexBufferCapacity;

	DrawVertex** m_VBDataPool;
	uint32_t m_VBDataPoolCapacity;
	bx::Mutex m_VBDataPoolMutex;

	DrawCommand* m_DrawCommands;
	uint32_t m_NumDrawCommands;
	uint32_t m_DrawCommandCapacity;

	Image* m_Images;
	uint32_t m_ImageCapacity;
	bx::HandleAllocT<MAX_TEXTURES> m_ImageAlloc;

	Vec2* m_PathVertices;
#if BATCH_TRANSFORM
	Vec2* m_TransformedPathVertices;
	bool m_PathVerticesTransformed;
#endif
	uint32_t m_NumPathVertices;
	uint32_t m_PathVertexCapacity;

#if BATCH_PATH_DIRECTIONS
	Vec2* m_PathDirections;
	uint32_t m_PathDirectionsCapacity;
#endif

	SubPath* m_SubPaths;
	uint32_t m_NumSubPaths;
	uint32_t m_SubPathCapacity;

	State m_StateStack[MAX_STATE_STACK_SIZE];
	uint32_t m_CurStateID;

	IndexBuffer* m_IndexBuffer;

	FONSquad* m_TextQuads;
	uint32_t m_TextQuadCapacity;

	FONScontext* m_FontStashContext;
	ImageHandle m_FontImages[MAX_FONT_IMAGES];
	int m_FontImageID;
	void* m_FontData[MAX_FONTS];
	uint32_t m_NextFontID;

	Gradient m_Gradients[MAX_GRADIENTS];
	bx::HandleAllocT<MAX_GRADIENTS> m_GradientAlloc;

	ImagePattern m_ImagePatterns[MAX_IMAGE_PATTERNS];
	bx::HandleAllocT<MAX_IMAGE_PATTERNS> m_ImagePatternAlloc;

	bgfx::VertexDecl m_DrawVertexDecl;
	bgfx::ProgramHandle m_ProgramHandle[DrawCommand::NumTypes];
	bgfx::UniformHandle m_TexUniform;
	bgfx::UniformHandle m_PaintMatUniform;
	bgfx::UniformHandle m_ExtentRadiusFeatherUniform;
	bgfx::UniformHandle m_InnerColorUniform;
	bgfx::UniformHandle m_OuterColorUniform;
	
	uint16_t m_WinWidth;
	uint16_t m_WinHeight;
	float m_DevicePixelRatio;
	float m_TesselationTolerance;
	float m_FringeWidth;
	uint8_t m_ViewID;
	bool m_ForceNewDrawCommand;

	Context(bx::AllocatorI* allocator, uint8_t viewID) : 
		m_Allocator(allocator),
		m_VertexBuffers(nullptr),
		m_NumVertexBuffers(0),
		m_VertexBufferCapacity(0),
		m_VBDataPool(nullptr),
		m_VBDataPoolCapacity(0),
		m_DrawCommands(nullptr),
		m_NumDrawCommands(0),
		m_DrawCommandCapacity(0),
		m_Images(nullptr),
		m_ImageCapacity(0),
		m_PathVertices(nullptr),
		m_NumPathVertices(0),
		m_PathVertexCapacity(0),
#if BATCH_TRANSFORM
		m_TransformedPathVertices(nullptr),
		m_PathVerticesTransformed(false),
#endif
#if BATCH_PATH_DIRECTIONS
		m_PathDirections(nullptr),
		m_PathDirectionsCapacity(0),
#endif
		m_SubPaths(nullptr),
		m_NumSubPaths(0),
		m_SubPathCapacity(0),
		m_CurStateID(0),
		m_IndexBuffer(nullptr),
		m_TextQuads(nullptr),
		m_TextQuadCapacity(0),
		m_FontStashContext(nullptr),
		m_FontImageID(0),
		m_WinWidth(0),
		m_WinHeight(0),
		m_DevicePixelRatio(1.0f),
		m_TesselationTolerance(1.0f),
		m_FringeWidth(1.0f),
		m_ViewID(viewID),
		m_ForceNewDrawCommand(false),
		m_NextFontID(0)
	{
		for (uint32_t i = 0; i < MAX_FONT_IMAGES; ++i) {
			m_FontImages[i] = BGFX_INVALID_HANDLE;
		}

		for (uint32_t i = 0; i < DrawCommand::NumTypes; ++i) {
			m_ProgramHandle[i] = BGFX_INVALID_HANDLE;
		}

		m_TexUniform = BGFX_INVALID_HANDLE;
		m_PaintMatUniform = BGFX_INVALID_HANDLE;
		m_ExtentRadiusFeatherUniform = BGFX_INVALID_HANDLE;
		m_InnerColorUniform = BGFX_INVALID_HANDLE;
		m_OuterColorUniform = BGFX_INVALID_HANDLE;

		memset(m_FontData, 0, sizeof(void*) * MAX_FONTS);
	}
	
	// Helpers...
	inline State* getState() 
	{ 
		return &m_StateStack[m_CurStateID]; 
	}

	inline SubPath* getSubPath()
	{
		if (m_NumSubPaths == 0) {
			return nullptr; 
		}

		return &m_SubPaths[m_NumSubPaths - 1];
	}
	
	inline VertexBuffer* getVertexBuffer() 
	{
		return &m_VertexBuffers[m_NumVertexBuffers - 1]; 
	}

	inline IndexBuffer* getIndexBuffer() 
	{
		return m_IndexBuffer; 
	}

	inline Vec2 getWhitePixelUV()
	{
		int w, h;
		getImageSize(m_FontImages[0], &w, &h);
		return Vec2(1.0f / (float)w, 1.0f / (float)h);
	}

	// Vertex buffers
	VertexBuffer* allocVertexBuffer();
	DrawVertex* allocVertexBufferData();
	void releaseVertexBufferData(DrawVertex* data);

	// Draw commands
	DrawCommand* allocDrawCommand_TexturedVertexColor(uint32_t numVertices, uint32_t numIndices, ImageHandle img);
	DrawCommand* allocDrawCommand_ColorGradient(uint32_t numVertices, uint32_t numIndices, GradientHandle gradient);

	// Paths
	void addPathVertex(const Vec2& p);
	Vec2* allocPathVertices(uint32_t n);

	// Concave polygon decomposition
	void decomposeAndFillConcavePolygon(const Vec2* points, uint32_t numPoints, Color col, bool aa);
	Vec2* allocTempPoints(uint32_t n);
	void freeTempPoints(Vec2* ptr);

	// Textures
	ImageHandle allocImage();
	ImageHandle createImageRGBA(uint32_t width, uint32_t height, uint32_t flags, const uint8_t* data);
	bool updateImage(ImageHandle img, int x, int y, int w, int h, const uint8_t* data);
	bool deleteImage(ImageHandle img);
	void getImageSize(ImageHandle image, int* w, int* h);

	// Fonts
	FontHandle loadFontFromMemory(const char* name, const uint8_t* data, uint32_t size);
	bool allocTextAtlas();
	void flushTextAtlasTexture();

	// Rendering
	void renderPathStrokeNoAA(const Vec2* vtx, uint32_t numVertices, bool isClosed, float thickness, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin);
	void renderPathStrokeAA(const Vec2* vtx, uint32_t numVertices, bool isClosed, float thickness, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin);
	void renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numVertices, Color color);
	void renderConvexPolygonAA(const Vec2* vtx, uint32_t numVertices, Color color);
	void renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numVertices, ImagePatternHandle img);
	void renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numVertices, GradientHandle grad);
	void renderTextQuads(FONSquad* quads, uint32_t numQuads, Color color);

#if BATCH_PATH_DIRECTIONS
	Vec2* allocPathDirections(uint32_t n)
	{
		if (n > m_PathDirectionsCapacity) {
			m_PathDirectionsCapacity = n;
			m_PathDirections = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_PathDirections, sizeof(Vec2) * m_PathDirectionsCapacity, 16);
		}

		return m_PathDirections;
	}
#endif
};

void releaseVertexBufferData(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseVertexBufferData((DrawVertex*)ptr);
}

inline Vec2 transformPos2D(float x, float y, const float* mtx)
{
	return Vec2(mtx[0] * x + mtx[2] * y + mtx[4],
	            mtx[1] * x + mtx[3] * y + mtx[5]);
}

inline Vec2 transformVec2D(float x, float y, const float* mtx)
{
	return Vec2(mtx[0] * x + mtx[2] * y,
	            mtx[1] * x + mtx[3] * y);
}

inline bool invertMatrix3(const float* t, float* inv)
{
	// nvgTransformInverse
	double invdet, det = (double)t[0] * t[3] - (double)t[2] * t[1];
	if (det > -1e-6 && det < 1e-6) {
		memset(inv, 0, sizeof(float) * 6);
		inv[0] = inv[2] = 1.0f;
		return false;
	}

	invdet = 1.0 / det;
	inv[0] = (float)(t[3] * invdet);
	inv[2] = (float)(-t[2] * invdet);
	inv[4] = (float)(((double)t[2] * t[5] - (double)t[3] * t[4]) * invdet);
	inv[1] = (float)(-t[1] * invdet);
	inv[3] = (float)(t[0] * invdet);
	inv[5] = (float)(((double)t[1] * t[4] - (double)t[0] * t[5]) * invdet);
	
	return true;
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

inline float getAverageScale(const float* t)
{
	// nvg__getAverageScale
	float sx = sqrtf(t[0] * t[0] + t[2] * t[2]);
	float sy = sqrtf(t[1] * t[1] + t[3] * t[3]);
	return (sx + sy) * 0.5f;
}

inline float quantize(float a, float d)
{
	return ((int)(a / d + 0.5f)) * d;
}

inline float getFontScale(const float* xform)
{
	return min2(quantize(getAverageScale(xform), 0.01f), 4.0f);
}

inline float rsqrt_carmack(float x)
{
	const float threehalfs = 1.5f;

	float x2 = x * 0.5f;
	float y = x;
	long i = *(long *)&y;
	i = 0x5f3759df - (i >> 1);
	y = *(float *)&i;
	y = y * (threehalfs - (x2 * y * y));
//	y  = y * (threehalfs - (x2 * y * y));

	return y;
}

inline void vec2Normalize(Vec2* v)
{
	float lenSqr = v->x * v->x + v->y * v->y;

#if APPROXIMATE_MATH
	float invLen = rsqrt_carmack(lenSqr);
#else
	float invLen = lenSqr < 1e-5f ? 0.0f : (1.0f / sqrtf(lenSqr));
#endif

	v->x *= invLen;
	v->y *= invLen;
}

inline Vec2 calcExtrusionVector(const Vec2& d01, const Vec2& d12)
{
	// Calculate vector 'v' from the equation P = p1 + v * w, where 'P' is the extruded point,
	// 'p1' is the current path vertex (see decl above) and 'w' is the extrusion width.
	// NanoVG calculates 'v' using the average of the 2 segment normals. BUT I DON'T UNDERSTAND
	// HOW ITS LENGTH IS DERIVED!!! ImDrawList seems to do the same thing.
	// 
	// So, in this case 'v' is calculated by the intersection of the 2 translated/extruded line segments.
	// Line segment 'p01' (from 'p0' to 'p1') is translated 'k' units to the left. Line segment 'p12' is also 
	// translated 'k' units to the left. The intersection point 'P' is then calculated from the 2 segments.
	// Since both strokes have the same width, equations can be simplified a lot. I ended up with the 
	// following equation for 'v':
	//
	// v = perpCCW(d01) + d01 * [(1.0 - dot(d12, d01)) / cross(d12, d01)]
	// 
	// where dxx are the unit direction vectors defined above.
	//
	// In the special case where d01 is parallel to d12, the cross product is zero and the 2nd term
	// is omitted (1.0 - dot() is also 0.0).
	//
	// Note that the above equation can be simplified further by expanding all 3 functions (dot/cross/perpCCW).
	// The final equation seems to be:
	// 
	// v = (d01 - d12) / cross(d12, d01)
#if 0
	Vec2 v = d01.perpCCW();
	const float cross = d12.cross(d01);

	// TODO: fabs() shouldn't be necessary here if the polygon has correct winding. Leave it until I manage
	// to fix/enforce ordering.
	if (fabsf(cross) > 1e-5f) {
		const float dot = d12.dot(d01);
		const float f = (1.0f - dot) / cross;
		v += d01 * f;
	}
#else
	Vec2 v;
	const float cross = d12.cross(d01);
	if (fabsf(cross) > 1e-5f) {
		v = (d01 - d12) * (1.0f / cross);
	} else {
		v = d01.perpCCW();
	}
#endif

	return v;
}

BGFXVGRenderer::BGFXVGRenderer() : m_Context(nullptr)
{
}

BGFXVGRenderer::~BGFXVGRenderer()
{
	bx::AllocatorI* allocator = m_Context->m_Allocator;

	for (uint32_t i = 0; i < DrawCommand::NumTypes; ++i) {
		if (bgfx::isValid(m_Context->m_ProgramHandle[i])) {
			bgfx::destroyProgram(m_Context->m_ProgramHandle[i]);
		}
	}

	bgfx::destroyUniform(m_Context->m_TexUniform);
	bgfx::destroyUniform(m_Context->m_PaintMatUniform);
	bgfx::destroyUniform(m_Context->m_ExtentRadiusFeatherUniform);
	bgfx::destroyUniform(m_Context->m_InnerColorUniform);
	bgfx::destroyUniform(m_Context->m_OuterColorUniform);

	for (uint32_t i = 0; i < m_Context->m_VertexBufferCapacity; ++i) {
		if (bgfx::isValid(m_Context->m_VertexBuffers[i].m_bgfxHandle)) {
			bgfx::destroyDynamicVertexBuffer(m_Context->m_VertexBuffers[i].m_bgfxHandle);
		}
	}
	BX_FREE(allocator, m_Context->m_VertexBuffers);

	for (uint32_t i = 0; i < m_Context->m_VBDataPoolCapacity; ++i) {
		DrawVertex* buffer = m_Context->m_VBDataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (DrawVertex*)((uintptr_t)buffer & ~1);
			BX_FREE(allocator, buffer);
		}
	}
	BX_FREE(allocator, m_Context->m_VBDataPool);

	BX_FREE(allocator, m_Context->m_DrawCommands);

	// Manually delete font data
	for (int i = 0; i < MAX_FONTS; ++i) {
		if (m_Context->m_FontData[i]) {
			BX_FREE(allocator, m_Context->m_FontData[i]);
		}
	}

	fonsDeleteInternal(m_Context->m_FontStashContext);

	for (uint32_t i = 0; i < MAX_FONT_IMAGES; ++i) {
		m_Context->deleteImage(m_Context->m_FontImages[i]);
	}

	BX_FREE(allocator, m_Context->m_SubPaths);
	BX_DELETE(allocator, m_Context->m_IndexBuffer);

	BX_FREE(allocator, m_Context->m_Images);

	BX_ALIGNED_FREE(allocator, m_Context->m_TextQuads, 16);
	BX_ALIGNED_FREE(allocator, m_Context->m_PathVertices, 16);
#if BATCH_TRANSFORM
	BX_ALIGNED_FREE(allocator, m_Context->m_TransformedPathVertices, 16);
#endif
#if BATCH_PATH_DIRECTIONS
	BX_ALIGNED_FREE(allocator, m_Context->m_PathDirections, 16);
#endif
	
	BX_DELETE(allocator, m_Context);
	m_Context = nullptr;
}

bool BGFXVGRenderer::init(uint8_t viewID, bx::AllocatorI* allocator)
{
	m_Context = (Context*)BX_NEW(allocator, Context)(allocator, viewID);
	if (!m_Context) {
		return false;
	}

	m_Context->m_DevicePixelRatio = 1.0f;
	m_Context->m_TesselationTolerance = 0.25f;
	m_Context->m_FringeWidth = 1.0f;
	m_Context->m_CurStateID = 0;
	m_Context->m_StateStack[0].m_GlobalAlpha = 1.0f;
	ResetScissor();
	LoadIdentity();

	m_Context->m_SubPathCapacity = 16;
	m_Context->m_SubPaths = (SubPath*)BX_ALLOC(allocator, sizeof(SubPath) * m_Context->m_SubPathCapacity);

	m_Context->m_IndexBuffer = BX_NEW(allocator, IndexBuffer)();

	m_Context->m_DrawVertexDecl.begin()
		.add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.end();

	bgfx::RendererType::Enum bgfxRendererType = bgfx::getRendererType();
	m_Context->m_ProgramHandle[DrawCommand::Type_TexturedVertexColor] =
		bgfx::createProgram(
			bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_solid_color"), 
			bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_solid_color"),
			true);

	m_Context->m_ProgramHandle[DrawCommand::Type_ColorGradient] =
		bgfx::createProgram(
			bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_gradient"), 
			bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_gradient"),
			true);

	m_Context->m_TexUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Int1, 1);
	m_Context->m_PaintMatUniform = bgfx::createUniform("u_paintMat", bgfx::UniformType::Mat3, 1);
	m_Context->m_ExtentRadiusFeatherUniform = bgfx::createUniform("u_extentRadiusFeather", bgfx::UniformType::Vec4, 1);
	m_Context->m_InnerColorUniform = bgfx::createUniform("u_innerCol", bgfx::UniformType::Vec4, 1);
	m_Context->m_OuterColorUniform = bgfx::createUniform("u_outerCol", bgfx::UniformType::Vec4, 1);

	// Init font stash
	FONSparams fontParams;
	memset(&fontParams, 0, sizeof(fontParams));
	fontParams.width = 512;
	fontParams.height = 512;
	fontParams.flags = FONS_ZERO_TOPLEFT;
	fontParams.renderCreate = nullptr;
	fontParams.renderUpdate = nullptr;
	fontParams.renderDraw = nullptr;
	fontParams.renderDelete = nullptr;
	fontParams.userPtr = nullptr;
	m_Context->m_FontStashContext = fonsCreateInternal(&fontParams);
	if (!m_Context->m_FontStashContext) {
		return false;
	}

	m_Context->m_FontImages[0] = m_Context->createImageRGBA(fontParams.width, fontParams.height, ImageFlags::Filter_Bilinear, nullptr);
	if (!isValid(m_Context->m_FontImages[0])) {
		return false;
	}
	m_Context->m_FontImageID = 0;

	return true;
}

void BGFXVGRenderer::BeginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio)
{
	assert(windowWidth > 0 && windowWidth < 65536);
	assert(windowHeight > 0 && windowHeight < 65536);
	m_Context->m_WinWidth = (uint16_t)windowWidth;
	m_Context->m_WinHeight = (uint16_t)windowHeight;
	m_Context->m_DevicePixelRatio = devicePixelRatio;
	m_Context->m_TesselationTolerance = 0.25f / devicePixelRatio;
	m_Context->m_FringeWidth = 1.0f / devicePixelRatio;

	assert(m_Context->m_CurStateID == 0);
	ResetScissor();
	LoadIdentity();

	m_Context->m_NumVertexBuffers = 0;
	m_Context->allocVertexBuffer();

	m_Context->m_IndexBuffer->reset();
	m_Context->m_NumDrawCommands = 0;
	m_Context->m_ForceNewDrawCommand = true;
}

void BGFXVGRenderer::EndFrame()
{
	assert(m_Context->m_CurStateID == 0);

	if (m_Context->m_NumDrawCommands == 0) {
		return;
	}

	m_Context->flushTextAtlasTexture();

	// Update bgfx vertex buffers...
	for (uint32_t iVB = 0; iVB < m_Context->m_NumVertexBuffers; ++iVB) {
		VertexBuffer* vb = &m_Context->m_VertexBuffers[iVB];

		if (!bgfx::isValid(vb->m_bgfxHandle)) {
			vb->m_bgfxHandle = bgfx::createDynamicVertexBuffer(MAX_VB_VERTICES, m_Context->m_DrawVertexDecl, 0);
		}

		const bgfx::Memory* mem = bgfx::makeRef(vb->m_Vertices, sizeof(DrawVertex) * vb->m_Count, releaseVertexBufferData, m_Context);
		bgfx::updateDynamicVertexBuffer(vb->m_bgfxHandle, 0, mem);

		// Null out the buffer. Will be allocated again from the pool on the next frame.
		vb->m_Vertices = nullptr;
	}

	// Update bgfx index buffer...
	bgfx::TransientIndexBuffer tib;
	uint32_t totalIndices = bgfx::getAvailTransientIndexBuffer(m_Context->m_IndexBuffer->m_Count);
	assert(totalIndices == m_Context->m_IndexBuffer->m_Count);
	bgfx::allocTransientIndexBuffer(&tib, totalIndices);
	memcpy(tib.data, &m_Context->m_IndexBuffer->m_Indices[0], sizeof(uint16_t) * totalIndices);

	float viewMtx[16];
	float projMtx[16];
	bx::mtxIdentity(viewMtx);
	bx::mtxOrtho(projMtx, 0.0f, m_Context->m_WinWidth, m_Context->m_WinHeight, 0.0f, 0.0f, 1.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(m_Context->m_ViewID, viewMtx, projMtx);

	const uint32_t numCommands = m_Context->m_NumDrawCommands;
	for (uint32_t iCmd = 0; iCmd < numCommands; ++iCmd) {
		DrawCommand* cmd = &m_Context->m_DrawCommands[iCmd];

		bgfx::setVertexBuffer(m_Context->m_VertexBuffers[cmd->m_VertexBufferID].m_bgfxHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setIndexBuffer(&tib, cmd->m_FirstIndexID, cmd->m_NumIndices);
		bgfx::setScissor((uint16_t)cmd->m_ScissorRect[0], (uint16_t)cmd->m_ScissorRect[1], (uint16_t)cmd->m_ScissorRect[2], (uint16_t)cmd->m_ScissorRect[3]);

		if (cmd->m_Type == DrawCommand::Type_TexturedVertexColor) {
			assert(isValid(cmd->m_ImageHandle));
			Image* tex = &m_Context->m_Images[cmd->m_ImageHandle.idx];
			bgfx::setTexture(0, m_Context->m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(
				BGFX_STATE_ALPHA_WRITE |
				BGFX_STATE_RGB_WRITE |
				BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));

			bgfx::submit(m_Context->m_ViewID, m_Context->m_ProgramHandle[DrawCommand::Type_TexturedVertexColor], cmdDepth, false);
		} else if (cmd->m_Type == DrawCommand::Type_ColorGradient) {
			assert(isValid(cmd->m_GradientHandle));
			Gradient* grad = &m_Context->m_Gradients[cmd->m_GradientHandle.idx];

			bgfx::setUniform(m_Context->m_PaintMatUniform, grad->m_Matrix, 1);
			bgfx::setUniform(m_Context->m_ExtentRadiusFeatherUniform, grad->m_Params, 1);
			bgfx::setUniform(m_Context->m_InnerColorUniform, grad->m_InnerColor, 1);
			bgfx::setUniform(m_Context->m_OuterColorUniform, grad->m_OuterColor, 1);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(
				BGFX_STATE_ALPHA_WRITE |
				BGFX_STATE_RGB_WRITE |
				BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));

			bgfx::submit(m_Context->m_ViewID, m_Context->m_ProgramHandle[DrawCommand::Type_ColorGradient], cmdDepth, false);
		} else {
			assert(false);
		}
	}

	// nvgEndFrame
	if (m_Context->m_FontImageID != 0) {
		ImageHandle fontImage = m_Context->m_FontImages[m_Context->m_FontImageID];

		// delete images that smaller than current one
		if (isValid(fontImage)) {
			int iw, ih;
			GetImageSize(fontImage, &iw, &ih);

			int j = 0;
			for (int i = 0; i < m_Context->m_FontImageID; i++) {
				if (isValid(m_Context->m_FontImages[i])) {
					int nw, nh;
					GetImageSize(m_Context->m_FontImages[i], &nw, &nh);

					if (nw < iw || nh < ih) {
						DeleteImage(m_Context->m_FontImages[i]);
					} else {
						m_Context->m_FontImages[j++] = m_Context->m_FontImages[i];
					}
				}
			}

			// make current font image to first
			m_Context->m_FontImages[j++] = m_Context->m_FontImages[0];
			m_Context->m_FontImages[0] = fontImage;
			m_Context->m_FontImageID = 0;

			// clear all images after j
			for (int i = j; i < MAX_FONT_IMAGES; i++) {
				m_Context->m_FontImages[i] = BGFX_INVALID_HANDLE;
			}
		}
	}

	m_Context->m_ImagePatternAlloc.reset();
	m_Context->m_GradientAlloc.reset();
}

void BGFXVGRenderer::BeginPath()
{
	m_Context->m_NumSubPaths = 0;
	m_Context->m_NumPathVertices = 0;
	m_Context->m_SubPaths[0].m_IsClosed = false;
	m_Context->m_SubPaths[0].m_NumVertices = 0;
	m_Context->m_SubPaths[0].m_FirstVertexID = 0;
#if BATCH_TRANSFORM
	m_Context->m_PathVerticesTransformed = false;
#endif
}

void BGFXVGRenderer::MoveTo(float x, float y)
{
	SubPath* path = m_Context->getSubPath();
	if (!path || path->m_NumVertices > 0) {
		assert(!path || (path && m_Context->m_NumPathVertices > 0));

		// Move on to the next sub path.
		if (m_Context->m_NumSubPaths + 1 > m_Context->m_SubPathCapacity) {
			m_Context->m_SubPathCapacity += 16;
			m_Context->m_SubPaths = (SubPath*)BX_REALLOC(m_Context->m_Allocator, m_Context->m_SubPaths, sizeof(SubPath) * m_Context->m_SubPathCapacity);
		}
		m_Context->m_NumSubPaths++;

		path = m_Context->getSubPath();
		path->m_IsClosed = false;
		path->m_NumVertices = 0;
		path->m_FirstVertexID = m_Context->m_NumPathVertices;
	}

#if BATCH_TRANSFORM
	m_Context->addPathVertex(Vec2(x, y));
#else
	m_Context->addPathVertex(transformPos2D(x, y, m_Context->getState()->m_TransformMtx));
#endif
}

void BGFXVGRenderer::LineTo(float x, float y)
{
	assert(m_Context->getSubPath()->m_NumVertices > 0);
#if BATCH_TRANSFORM
	m_Context->addPathVertex(Vec2(x, y));
#else
	m_Context->addPathVertex(transformPos2D(x, y, m_Context->getState()->m_TransformMtx));
#endif
}

void BGFXVGRenderer::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	State* state = m_Context->getState();
	SubPath* path = m_Context->getSubPath();
	assert(path->m_NumVertices > 0);

#if BATCH_TRANSFORM
	const Vec2* lastVertex = &m_Context->m_PathVertices[path->m_FirstVertexID + path->m_NumVertices - 1];
	float x1 = lastVertex->x;
	float y1 = lastVertex->y;
	float x2 = c1x;
	float y2 = c1y;
	float x3 = c2x;
	float y3 = c2y;
	float x4 = x;
	float y4 = y;

	const float avgScale = state->m_AvgScale;
	const float tessTol = m_Context->m_TesselationTolerance / (avgScale * avgScale);
#else
	const float* mtx = state->m_TransformMtx;
	Vec2 p2 = transformPos2D(c1x, c1y, mtx);
	Vec2 p3 = transformPos2D(c2x, c2y, mtx);
	Vec2 p4 = transformPos2D(x, y, mtx);

	const Vec2* lastVertex = &m_Context->m_PathVertices[path->m_FirstVertexID + path->m_NumVertices - 1];
	float x1 = lastVertex->x;
	float y1 = lastVertex->y;
	float x2 = p2.x;
	float y2 = p2.y;
	float x3 = p3.x;
	float y3 = p3.y;
	float x4 = p4.x;
	float y4 = p4.y;

	const float tessTol = m_Context->m_TesselationTolerance;
#endif

	const int MAX_LEVELS = 10;
	float* stack = (float*)alloca(sizeof(float) * 8 * MAX_LEVELS);
	float* stackPtr = stack;
	while (true) {
		float dx = x4 - x1;
		float dy = y4 - y1;
		float d2 = fabsf(((x2 - x4) * dy - (y2 - y4) * dx));
		float d3 = fabsf(((x3 - x4) * dy - (y3 - y4) * dx));

		if ((d2 + d3) * (d2 + d3) <= tessTol * (dx * dx + dy * dy)) {
			m_Context->addPathVertex(Vec2(x4, y4));

			// Pop sibling off the stack and decrease level...
			if (stackPtr == stack) {
				break;
			}

			x1 = *(--stackPtr);
			y1 = *(--stackPtr);
			x2 = *(--stackPtr);
			y2 = *(--stackPtr);
			x3 = *(--stackPtr);
			y3 = *(--stackPtr);
			x4 = *(--stackPtr);
			y4 = *(--stackPtr);
		} else {
			const ptrdiff_t curLevel = (stackPtr - stack); // 8 floats per sub-curve
			if (curLevel < MAX_LEVELS * 8) {
				float x12 = (x1 + x2) * 0.5f;
				float y12 = (y1 + y2) * 0.5f;
				float x23 = (x2 + x3) * 0.5f;
				float y23 = (y2 + y3) * 0.5f;
				float x34 = (x3 + x4) * 0.5f;
				float y34 = (y3 + y4) * 0.5f;
				float x123 = (x12 + x23) * 0.5f;
				float y123 = (y12 + y23) * 0.5f;
				float x234 = (x23 + x34) * 0.5f;
				float y234 = (y23 + y34) * 0.5f;
				float x1234 = (x123 + x234) * 0.5f;
				float y1234 = (y123 + y234) * 0.5f;

				// Push sibling on the stack...
				*stackPtr++ = y4;
				*stackPtr++ = x4;
				*stackPtr++ = y34;
				*stackPtr++ = x34;
				*stackPtr++ = y234;
				*stackPtr++ = x234;
				*stackPtr++ = y1234;
				*stackPtr++ = x1234;

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
				x1 = *(--stackPtr);
				y1 = *(--stackPtr);
				x2 = *(--stackPtr);
				y2 = *(--stackPtr);
				x3 = *(--stackPtr);
				y3 = *(--stackPtr);
				x4 = *(--stackPtr);
				y4 = *(--stackPtr);
			}
		}
	}
}

void BGFXVGRenderer::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	BX_UNUSED(x1, y1, x2, y2, radius);
	assert(false); // NOT USED
}

void BGFXVGRenderer::Rect(float x, float y, float w, float h)
{
	if (fabsf(w) < 1e-5f || fabsf(h) < 1e-5f) {
		return;
	}

	// nvgRect
	// CCW order
	MoveTo(x, y);
	LineTo(x, y + h);
	LineTo(x + w, y + h);
	LineTo(x + w, y);
	ClosePath();
}

void BGFXVGRenderer::RoundedRect(float x, float y, float w, float h, float r)
{
	// nvgRoundedRect
	if (r < 0.1f) {
		Rect(x, y, w, h);
	} else {
		float rx = min2(r, fabsf(w) * 0.5f) * sign(w);
		float ry = min2(r, fabsf(h) * 0.5f) * sign(h);

#if BEZIER_CIRCLE
		MoveTo(x, y + ry);
		LineTo(x, y + h - ry);
		BezierTo(x, y + h - ry*(1 - NVG_KAPPA90), x + rx*(1 - NVG_KAPPA90), y + h, x + rx, y + h);
		LineTo(x + w - rx, y + h);
		BezierTo(x + w - rx*(1 - NVG_KAPPA90), y + h, x + w, y + h - ry*(1 - NVG_KAPPA90), x + w, y + h - ry);
		LineTo(x + w, y + ry);
		BezierTo(x + w, y + ry*(1 - NVG_KAPPA90), x + w - rx*(1 - NVG_KAPPA90), y, x + w - rx, y);
		LineTo(x + rx, y);
		BezierTo(x + rx*(1 - NVG_KAPPA90), y, x, y + ry*(1 - NVG_KAPPA90), x, y + ry);
		ClosePath();
#else
		r = min2(rx, ry);

		State* state = m_Context->getState();

		const float scale = state->m_AvgScale;
#if APPROXIMATE_MATH
		const float da = approxAcos((scale * r) / ((scale * r) + m_Context->m_TesselationTolerance)) * 2.0f;
#else
		const float da = acos((scale * r) / ((scale * r) + m_Context->m_TesselationTolerance)) * 2.0f;
#endif
		const uint32_t numPointsHalfCircle = max2(2, (int)ceilf(PI / da));
		const uint32_t numPointsQuarterCircle = (numPointsHalfCircle >> 1) + 1;

		MoveTo(x, y + ry);
		LineTo(x, y + h - ry);

		SubPath* path = m_Context->getSubPath();

		// Bottom left quarter circle
		{
			float cx = x + rx;
			float cy = y + h - ry;
			Vec2* circleVertices = m_Context->allocPathVertices(numPointsQuarterCircle - 1);
			for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
				const float a = -PI - (PI * 0.5f) * ((float)i / (float)(numPointsQuarterCircle - 1));

#if APPROXIMATE_MATH
				const float ca = approxCos(a);
				const float sa = approxSin(a);
#else
				const float ca = cosf(a);
				const float sa = sinf(a);
#endif

#if BATCH_TRANSFORM
				*circleVertices++ = Vec2(cx + r * ca, cy + r * sa);
#else
				*circleVertices++ = transformPos2D(cx + r * ca, cy + r * sa, mtx);
#endif
			}
			path->m_NumVertices += (numPointsQuarterCircle - 1);
		}

		LineTo(x + w - rx, y + h);

		// Bottom right quarter circle
		{
			float cx = x + w - rx;
			float cy = y + h - ry;
			Vec2* circleVertices = m_Context->allocPathVertices(numPointsQuarterCircle - 1);
			for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
				const float a = -1.5f * PI - (PI * 0.5f) * ((float)i / (float)(numPointsQuarterCircle - 1));

#if APPROXIMATE_MATH
				const float ca = approxCos(a);
				const float sa = approxSin(a);
#else
				const float ca = cosf(a);
				const float sa = sinf(a);
#endif

#if BATCH_TRANSFORM
				*circleVertices++ = Vec2(cx + r * ca, cy + r * sa);
#else
				*circleVertices++ = transformPos2D(cx + r * ca, cy + r * sa, mtx);
#endif
			}
			path->m_NumVertices += (numPointsQuarterCircle - 1);
		}

		LineTo(x + w, y + ry);

		// Top right quarter circle
		{
			float cx = x + w - rx;
			float cy = y + ry;
			Vec2* circleVertices = m_Context->allocPathVertices(numPointsQuarterCircle - 1);
			for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
				const float a = -(PI * 0.5f) * ((float)i / (float)(numPointsQuarterCircle - 1));

#if APPROXIMATE_MATH
				const float ca = approxCos(a);
				const float sa = approxSin(a);
#else
				const float ca = cosf(a);
				const float sa = sinf(a);
#endif

#if BATCH_TRANSFORM
				*circleVertices++ = Vec2(cx + r * ca, cy + r * sa);
#else
				*circleVertices++ = transformPos2D(cx + r * ca, cy + r * sa, mtx);
#endif
			}
			path->m_NumVertices += (numPointsQuarterCircle - 1);
		}

		LineTo(x + rx, y);

		// Top left quarter circle
		{
			float cx = x + rx;
			float cy = y + ry;
			Vec2* circleVertices = m_Context->allocPathVertices(numPointsQuarterCircle - 1);
			for (uint32_t i = 1; i < numPointsQuarterCircle; ++i) {
				const float a = -PI * 0.5f - (PI * 0.5f) * ((float)i / (float)(numPointsQuarterCircle - 1));

#if APPROXIMATE_MATH
				const float ca = approxCos(a);
				const float sa = approxSin(a);
#else
				const float ca = cosf(a);
				const float sa = sinf(a);
#endif

#if BATCH_TRANSFORM
				*circleVertices++ = Vec2(cx + r * ca, cy + r * sa);
#else
				*circleVertices++ = transformPos2D(cx + r * ca, cy + r * sa, mtx);
#endif
			}
			path->m_NumVertices += (numPointsQuarterCircle - 1);
		}

		ClosePath();
#endif
	}
}

void BGFXVGRenderer::Circle(float cx, float cy, float r)
{
#if BEZIER_CIRCLE
	// TODO: This ends up appearing in the profiler because we are rendering many component pins. 
	// Check if it'd be better to draw a circle directly and somehow calculate the number of segments based on current 
	// scale.
	// 
	// nvgEllipse with both radii equal to r
	// See: http://spencermortensen.com/articles/bezier-circle/
	MoveTo(cx - r, cy);
	BezierTo(cx - r, cy + r * NVG_KAPPA90, cx - r * NVG_KAPPA90, cy + r, cx, cy + r);
	BezierTo(cx + r * NVG_KAPPA90, cy + r, cx + r, cy + r * NVG_KAPPA90, cx + r, cy);
	BezierTo(cx + r, cy - r * NVG_KAPPA90, cx + r * NVG_KAPPA90, cy - r, cx, cy - r);
	BezierTo(cx - r * NVG_KAPPA90, cy - r, cx - r, cy - r * NVG_KAPPA90, cx - r, cy);
	ClosePath();
#else
	// NOTE: It still appears in the profiler with a slightly lower percentage.
	State* state = m_Context->getState();
#if !BATCH_TRANSFORM
	const float* mtx = state->m_TransformMtx;
#endif
	const float scale = state->m_AvgScale;
#if APPROXIMATE_MATH
	const float da = approxAcos((scale * r) / ((scale * r) + m_Context->m_TesselationTolerance)) * 2.0f;
#else
	const float da = acos((scale * r) / ((scale * r) + m_Context->m_TesselationTolerance)) * 2.0f;
#endif
	const uint32_t numPointsHalfCircle = max2(2, (int)ceilf(PI / da));

	const uint32_t numPoints = (numPointsHalfCircle * 2);

	MoveTo(cx + r, cy);

	// NOTE: Get the subpath after MoveTo() because MoveTo will handle the creation of the new subpath (if needed).
	SubPath* path = m_Context->getSubPath();
	Vec2* circleVertices = m_Context->allocPathVertices(numPoints - 1);
	for (uint32_t i = 1; i < numPoints; ++i) {
		const float a = (2.0f * PI) * (1.0f - (float)i / (float)numPoints);
		
#if APPROXIMATE_MATH
		const float ca = approxCos(a);
		const float sa = approxSin(a);
#else
		const float ca = cosf(a);
		const float sa = sinf(a);
#endif

#if BATCH_TRANSFORM
		*circleVertices++ = Vec2(cx + r * ca, cy + r * sa);
#else
		*circleVertices++ = transformPos2D(cx + r * ca, cy + r * sa, mtx);
#endif
	}
	path->m_NumVertices += (numPoints - 1);
	ClosePath();
#endif
}

void BGFXVGRenderer::ClosePath()
{
	SubPath* path = m_Context->getSubPath();
	assert(!path->m_IsClosed);
	assert(path->m_NumVertices > 2);
	path->m_IsClosed = true;

	const Vec2& first = m_Context->m_PathVertices[path->m_FirstVertexID];
	const Vec2& last = m_Context->m_PathVertices[path->m_FirstVertexID + path->m_NumVertices - 1];
	Vec2 d = first - last;
	if (d.dot(d) < 1e-5f) {
		--path->m_NumVertices;
		--m_Context->m_NumPathVertices;
	}
}

#if BATCH_PATH_DIRECTIONS
void calcPathDirectionsSSE(const Vec2* __restrict v, uint32_t n, Vec2* __restrict d, bool isClosed)
{
	const bx::simd128_t eps = bx::simd_splat(1e-5f);

	const float* src = &v->x;
	float* dst = &d->x;

	if (n == 2) {
		assert(!isClosed);

		bx::simd128_t src0123 = bx::simd_ld(src);
		bx::simd128_t src0101 = bx::simd_swiz_xyxy(src0123);
		bx::simd128_t src2323 = bx::simd_swiz_zwzw(src0123);

		bx::simd128_t d20_31_20_31 = bx::simd_sub(src2323, src0101);
		bx::simd128_t d20_31_20_31_sqr = bx::simd_mul(d20_31_20_31, d20_31_20_31);
		bx::simd128_t d31_20_31_20_sqr = bx::simd_swiz_yxzw(d20_31_20_31_sqr);
		bx::simd128_t l2 = bx::simd_add(d20_31_20_31_sqr, d31_20_31_20_sqr);
		bx::simd128_t mask = bx::simd_cmpgt(l2, eps);
#if APPROXIMATE_MATH
		bx::simd128_t inv_l = bx::simd_rsqrt_carmack(l2);
#else
		bx::simd128_t inv_l = bx::simd_rsqrt(l2);
#endif
		bx::simd128_t inv_l_masked = bx::simd_and(inv_l, mask);

		bx::simd128_t inv_l_0011 = bx::simd_swiz_xxyy(inv_l_masked);

		bx::simd128_t dn_01 = bx::simd_mul(d20_31_20_31, inv_l_0011);

		bx::simd_st(dst, dn_01);

		assert((d[0] - d[1]).length() == 0.0f);
	} else {
		const uint32_t iter = (n >> 2) - ((n & 3) > 1 ? 0 : 1);
		for (uint32_t i = 0; i < iter; ++i) {
			bx::simd128_t src0123 = bx::simd_ld(src);
			bx::simd128_t src4567 = bx::simd_ld(src + 4);
			bx::simd128_t src89xx = bx::simd_ld(src + 8);

			bx::simd128_t src2323 = bx::simd_swiz_zwzw(src0123);
			bx::simd128_t src6767 = bx::simd_swiz_zwzw(src4567);

			bx::simd128_t src2345 = bx::simd_shuf_xyAB(src2323, src4567);
			bx::simd128_t src6789 = bx::simd_shuf_xyAB(src6767, src89xx);

			bx::simd128_t d20_31_42_53 = bx::simd_sub(src2345, src0123);
			bx::simd128_t d64_75_86_97 = bx::simd_sub(src6789, src4567);

			bx::simd128_t d20_31_42_53_sqr = bx::simd_mul(d20_31_42_53, d20_31_42_53);
			bx::simd128_t d64_75_86_97_sqr = bx::simd_mul(d64_75_86_97, d64_75_86_97);

			bx::simd128_t d20_42_20_42_sqr = bx::simd_swiz_xzxz(d20_31_42_53_sqr);
			bx::simd128_t d31_53_31_53_sqr = bx::simd_swiz_ywyw(d20_31_42_53_sqr);
			bx::simd128_t d64_86_64_86_sqr = bx::simd_swiz_xzxz(d64_75_86_97_sqr);
			bx::simd128_t d75_97_75_97_sqr = bx::simd_swiz_ywyw(d64_75_86_97_sqr);

			bx::simd128_t d20_42_64_86_sqr = bx::simd_shuf_xyAB(d20_42_20_42_sqr, d64_86_64_86_sqr);
			bx::simd128_t d31_53_75_97_sqr = bx::simd_shuf_xyAB(d31_53_31_53_sqr, d75_97_75_97_sqr);

			bx::simd128_t l2 = bx::simd_add(d20_42_64_86_sqr, d31_53_75_97_sqr);

			bx::simd128_t mask = bx::simd_cmpgt(l2, eps);
#if APPROXIMATE_MATH
			bx::simd128_t inv_l = bx::simd_rsqrt_carmack(l2);
#else
			bx::simd128_t inv_l = bx::simd_rsqrt(l2);
#endif
			bx::simd128_t inv_l_masked = bx::simd_and(inv_l, mask);

			bx::simd128_t inv_l_0011 = bx::simd_swiz_xxyy(inv_l_masked);
			bx::simd128_t inv_l_2233 = bx::simd_swiz_zzww(inv_l_masked);
			bx::simd128_t dn_01 = bx::simd_mul(d20_31_42_53, inv_l_0011);
			bx::simd128_t dn_23 = bx::simd_mul(d64_75_86_97, inv_l_2233);

			bx::simd_st(dst, dn_01);
			bx::simd_st(dst + 4, dn_23);

			src += 8;
			dst += 8;
		}

		Vec2* dstVec = (Vec2*)dst;
		const Vec2* srcVec = (const Vec2*)src;
		uint32_t rem = n - (iter << 2) - 1;
		while (rem-- > 0) {
			*dstVec = srcVec[1] - srcVec[0];
			vec2Normalize(dstVec);
			dstVec++;
			srcVec++;
		}

		if (isClosed) {
			*dstVec = v[0] - srcVec[0];
			vec2Normalize(dstVec);
		} else {
			*dstVec = *(dstVec - 1);
		}
	}
}
#endif // BATCH_PATH_DIRECTIONS

#if BATCH_TRANSFORM
void transformPointsSSE(const Vec2* __restrict v, uint32_t n, Vec2* __restrict p, const float* __restrict mtx)
{
	const float* src = &v->x;
	float* dst = &p->x;

	const bx::simd128_t mtx0 = bx::simd_splat(mtx[0]);
	const bx::simd128_t mtx1 = bx::simd_splat(mtx[1]);
	const bx::simd128_t mtx2 = bx::simd_splat(mtx[2]);
	const bx::simd128_t mtx3 = bx::simd_splat(mtx[3]);
	const bx::simd128_t mtx4 = bx::simd_splat(mtx[4]);
	const bx::simd128_t mtx5 = bx::simd_splat(mtx[5]);

	const uint32_t iter = n >> 2;
	for (uint32_t i = 0; i < iter; ++i) {
		bx::simd128_t src0123 = bx::simd_ld(src);
		bx::simd128_t src4567 = bx::simd_ld(src + 4);

		bx::simd128_t src0246 = bx::simd_shuf_xyAB(bx::simd_swiz_xzxz(src0123), bx::simd_swiz_xzxz(src4567));
		bx::simd128_t src1357 = bx::simd_shuf_xyAB(bx::simd_swiz_ywyw(src0123), bx::simd_swiz_ywyw(src4567));

		bx::simd128_t dst0246 = bx::simd_add(bx::simd_add(bx::simd_mul(src0246, mtx0), bx::simd_mul(src1357, mtx2)), mtx4);
		bx::simd128_t dst1357 = bx::simd_add(bx::simd_add(bx::simd_mul(src0246, mtx1), bx::simd_mul(src1357, mtx3)), mtx5);

		bx::simd128_t dst0123 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(dst0246, dst1357));
		bx::simd128_t dst4567 = bx::simd_swiz_xzyw(bx::simd_shuf_zwCD(dst0246, dst1357));

		bx::simd_st(dst, dst0123);
		bx::simd_st(dst + 4, dst4567);

		src += 8;
		dst += 8;
	}

	const uint32_t rem = n & 3;
	switch (rem) {
	case 3:
		*dst++ = mtx[0] * src[0] + mtx[2] * src[1] + mtx[4];
		*dst++ = mtx[1] * src[0] + mtx[3] * src[1] + mtx[5];
		src += 2;
	case 2:
		*dst++ = mtx[0] * src[0] + mtx[2] * src[1] + mtx[4];
		*dst++ = mtx[1] * src[0] + mtx[3] * src[1] + mtx[5];
		src += 2;
	case 1:
		*dst++ = mtx[0] * src[0] + mtx[2] * src[1] + mtx[4];
		*dst++ = mtx[1] * src[0] + mtx[3] * src[1] + mtx[5];
		src += 2;
	}
}
#endif // BATCH_TRANSFORM

void BGFXVGRenderer::FillConvexPath(Color col, bool aa)
{
#if BATCH_TRANSFORM
	if (!m_Context->m_PathVerticesTransformed) {
		State* state = m_Context->getState();
		transformPointsSSE(m_Context->m_PathVertices, m_Context->m_NumPathVertices, m_Context->m_TransformedPathVertices, state->m_TransformMtx);
		m_Context->m_PathVerticesTransformed = true;
	}

	const Vec2* pathVertices = m_Context->m_TransformedPathVertices;
#else
	const Vec2* pathVertices = m_Context->m_PathVertices;
#endif

	const uint32_t numPaths = m_Context->m_NumSubPaths;
	for (uint32_t iPath = 0; iPath < numPaths; ++iPath) {
		const SubPath* path = &m_Context->m_SubPaths[iPath];
		if (path->m_NumVertices < 3) {
			continue;
		}

		if (aa) {
			m_Context->renderConvexPolygonAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, col);
		} else {
			m_Context->renderConvexPolygonNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, col);
		}
	}
}

void BGFXVGRenderer::FillConvexPath(GradientHandle gradient, bool aa)
{
#if BATCH_TRANSFORM
	if (!m_Context->m_PathVerticesTransformed) {
		State* state = m_Context->getState();
		transformPointsSSE(m_Context->m_PathVertices, m_Context->m_NumPathVertices, m_Context->m_TransformedPathVertices, state->m_TransformMtx);
		m_Context->m_PathVerticesTransformed = true;
	}

	const Vec2* pathVertices = m_Context->m_TransformedPathVertices;
#else
	const Vec2* pathVertices = m_Context->m_PathVertices;
#endif

	// TODO: Anti-aliasing of gradient-filled paths
	BX_UNUSED(aa);
	
	const uint32_t numPaths = m_Context->m_NumSubPaths;
	for (uint32_t iPath = 0; iPath < numPaths; ++iPath) {
		const SubPath* path = &m_Context->m_SubPaths[iPath];
		if (path->m_NumVertices < 3) {
			continue;
		}

		m_Context->renderConvexPolygonNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, gradient);
	}
}

void BGFXVGRenderer::FillConvexPath(ImagePatternHandle img, bool aa)
{
#if BATCH_TRANSFORM
	if (!m_Context->m_PathVerticesTransformed) {
		State* state = m_Context->getState();
		transformPointsSSE(m_Context->m_PathVertices, m_Context->m_NumPathVertices, m_Context->m_TransformedPathVertices, state->m_TransformMtx);
		m_Context->m_PathVerticesTransformed = true;
	}

	const Vec2* pathVertices = m_Context->m_TransformedPathVertices;
#else
	const Vec2* pathVertices = m_Context->m_PathVertices;
#endif

	// TODO: Anti-aliasing of textured paths
	BX_UNUSED(aa);
	const uint32_t numPaths = m_Context->m_NumSubPaths;
	for (uint32_t iPath = 0; iPath < numPaths; ++iPath) {
		const SubPath* path = &m_Context->m_SubPaths[iPath];
		if (path->m_NumVertices < 3) {
			continue;
		}

		m_Context->renderConvexPolygonNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, img);
	}
}

void BGFXVGRenderer::FillConcavePath(Color col, bool aa)
{
#if BATCH_TRANSFORM
	if (!m_Context->m_PathVerticesTransformed) {
		State* state = m_Context->getState();
		transformPointsSSE(m_Context->m_PathVertices, m_Context->m_NumPathVertices, m_Context->m_TransformedPathVertices, state->m_TransformMtx);
		m_Context->m_PathVerticesTransformed = true;
	}

	const Vec2* pathVertices = m_Context->m_TransformedPathVertices;
#else
	const Vec2* pathVertices = m_Context->m_PathVertices;
#endif

	const uint32_t numPaths = m_Context->m_NumSubPaths;
	assert(numPaths == 1); // Only a single concave polygon can be decomposed at a time.

	const SubPath* path = &m_Context->m_SubPaths[0];
	if (path->m_NumVertices < 3) {
		return;
	}

	m_Context->decomposeAndFillConcavePolygon(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, col, aa);
}

void BGFXVGRenderer::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
#if BATCH_TRANSFORM
	if (!m_Context->m_PathVerticesTransformed) {
		State* state = m_Context->getState();
		transformPointsSSE(m_Context->m_PathVertices, m_Context->m_NumPathVertices, m_Context->m_TransformedPathVertices, state->m_TransformMtx);
		m_Context->m_PathVerticesTransformed = true;
	}

	const Vec2* pathVertices = m_Context->m_TransformedPathVertices;
#else
	const Vec2* pathVertices = m_Context->m_PathVertices;
#endif
	
	const uint32_t numPaths = m_Context->m_NumSubPaths;
	for (uint32_t iSubPath = 0; iSubPath < numPaths; ++iSubPath) {
		SubPath* path = &m_Context->m_SubPaths[iSubPath];
		if (path->m_NumVertices < 2) {
			continue;
		}

		if (aa) {
			m_Context->renderPathStrokeAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, path->m_IsClosed, width, col, lineCap, lineJoin);
		} else {
			m_Context->renderPathStrokeNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, path->m_IsClosed, width, col, lineCap, lineJoin);
		}
	}
}

// NOTE: In contrast to NanoVG these Gradients are State dependent (the current transformation matrix is baked in the Gradient matrix).
GradientHandle BGFXVGRenderer::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	// nvgLinearGradient()
	GradientHandle handle = { m_Context->m_GradientAlloc.alloc() };
	if (handle.idx == bx::HandleAlloc::invalid) {
		return handle;
	}

	State* state = m_Context->getState();

	const float large = 1e5;
	float dx = ex - sx;
	float dy = ey - sy;
	float d = sqrtf(dx * dx + dy * dy);
	if (d > 0.0001f) {
		dx /= d;
		dy /= d;
	} else {
		dx = 0;
		dy = 1;
	}

	float gradientMatrix[6];
	gradientMatrix[0] = dy;
	gradientMatrix[1] = -dx;
	gradientMatrix[2] = dx;
	gradientMatrix[3] = dy;
	gradientMatrix[4] = sx - dx * large;
	gradientMatrix[5] = sy - dy * large;

	float patternMatrix[6];
	multiplyMatrix3(state->m_TransformMtx, gradientMatrix, patternMatrix);

	float inversePatternMatrix[6];
	invertMatrix3(patternMatrix, inversePatternMatrix);

	Gradient* grad = &m_Context->m_Gradients[handle.idx];
	grad->m_Matrix[0] = inversePatternMatrix[0];
	grad->m_Matrix[1] = inversePatternMatrix[1];
	grad->m_Matrix[2] = 0.0f;
	grad->m_Matrix[3] = inversePatternMatrix[2];
	grad->m_Matrix[4] = inversePatternMatrix[3];
	grad->m_Matrix[5] = 0.0f;
	grad->m_Matrix[6] = inversePatternMatrix[4];
	grad->m_Matrix[7] = inversePatternMatrix[5];
	grad->m_Matrix[8] = 1.0f;
	grad->m_Params[0] = large;
	grad->m_Params[1] = large + d * 0.5f;
	grad->m_Params[2] = 0.0f;
	grad->m_Params[3] = max2(1.0f, d);
	grad->m_InnerColor[0] = ColorRGBA::getRed(icol) / 255.0f;
	grad->m_InnerColor[1] = ColorRGBA::getGreen(icol) / 255.0f;
	grad->m_InnerColor[2] = ColorRGBA::getBlue(icol) / 255.0f;
	grad->m_InnerColor[3] = ColorRGBA::getAlpha(icol) / 255.0f;
	grad->m_OuterColor[0] = ColorRGBA::getRed(ocol) / 255.0f;
	grad->m_OuterColor[1] = ColorRGBA::getGreen(ocol) / 255.0f;
	grad->m_OuterColor[2] = ColorRGBA::getBlue(ocol) / 255.0f;
	grad->m_OuterColor[3] = ColorRGBA::getAlpha(ocol) / 255.0f;

	return handle;
}

GradientHandle BGFXVGRenderer::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	GradientHandle handle = { m_Context->m_GradientAlloc.alloc() };
	if (handle.idx == bx::HandleAlloc::invalid) {
		return handle;
	}

	State* state = m_Context->getState();

	float gradientMatrix[6];
	gradientMatrix[0] = 1.0f;
	gradientMatrix[1] = 0.0f;
	gradientMatrix[2] = 0.0f;
	gradientMatrix[3] = 1.0f;
	gradientMatrix[4] = x + w * 0.5f;
	gradientMatrix[5] = y + h * 0.5f;

	float patternMatrix[6];
	multiplyMatrix3(state->m_TransformMtx, gradientMatrix, patternMatrix);

	float inversePatternMatrix[6];
	invertMatrix3(patternMatrix, inversePatternMatrix);

	Gradient* grad = &m_Context->m_Gradients[handle.idx];
	grad->m_Matrix[0] = inversePatternMatrix[0];
	grad->m_Matrix[1] = inversePatternMatrix[1];
	grad->m_Matrix[2] = 0.0f;
	grad->m_Matrix[3] = inversePatternMatrix[2];
	grad->m_Matrix[4] = inversePatternMatrix[3];
	grad->m_Matrix[5] = 0.0f;
	grad->m_Matrix[6] = inversePatternMatrix[4];
	grad->m_Matrix[7] = inversePatternMatrix[5];
	grad->m_Matrix[8] = 1.0f;
	grad->m_Params[0] = w * 0.5f;
	grad->m_Params[1] = h * 0.5f;
	grad->m_Params[2] = r;
	grad->m_Params[3] = max2(1.0f, f);
	grad->m_InnerColor[0] = ColorRGBA::getRed(icol) / 255.0f;
	grad->m_InnerColor[1] = ColorRGBA::getGreen(icol) / 255.0f;
	grad->m_InnerColor[2] = ColorRGBA::getBlue(icol) / 255.0f;
	grad->m_InnerColor[3] = ColorRGBA::getAlpha(icol) / 255.0f;
	grad->m_OuterColor[0] = ColorRGBA::getRed(ocol) / 255.0f;
	grad->m_OuterColor[1] = ColorRGBA::getGreen(ocol) / 255.0f;
	grad->m_OuterColor[2] = ColorRGBA::getBlue(ocol) / 255.0f;
	grad->m_OuterColor[3] = ColorRGBA::getAlpha(ocol) / 255.0f;

	return handle;
}

GradientHandle BGFXVGRenderer::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	GradientHandle handle = { m_Context->m_GradientAlloc.alloc() };
	if (handle.idx == bx::HandleAlloc::invalid) {
		return handle;
	}

	State* state = m_Context->getState();

	float gradientMatrix[6];
	gradientMatrix[0] = 1.0f;
	gradientMatrix[1] = 0.0f;
	gradientMatrix[2] = 0.0f;
	gradientMatrix[3] = 1.0f;
	gradientMatrix[4] = cx;
	gradientMatrix[5] = cy;

	float patternMatrix[6];
	multiplyMatrix3(state->m_TransformMtx, gradientMatrix, patternMatrix);

	float inversePatternMatrix[6];
	invertMatrix3(patternMatrix, inversePatternMatrix);

	float r = (inr + outr) * 0.5f;
	float f = (outr - inr);

	Gradient* grad = &m_Context->m_Gradients[handle.idx];
	grad->m_Matrix[0] = inversePatternMatrix[0];
	grad->m_Matrix[1] = inversePatternMatrix[1];
	grad->m_Matrix[2] = 0.0f;
	grad->m_Matrix[3] = inversePatternMatrix[2];
	grad->m_Matrix[4] = inversePatternMatrix[3];
	grad->m_Matrix[5] = 0.0f;
	grad->m_Matrix[6] = inversePatternMatrix[4];
	grad->m_Matrix[7] = inversePatternMatrix[5];
	grad->m_Matrix[8] = 1.0f;
	grad->m_Params[0] = r * 0.5f;
	grad->m_Params[1] = r * 0.5f;
	grad->m_Params[2] = r;
	grad->m_Params[3] = max2(1.0f, f);
	grad->m_InnerColor[0] = ColorRGBA::getRed(icol) / 255.0f;
	grad->m_InnerColor[1] = ColorRGBA::getGreen(icol) / 255.0f;
	grad->m_InnerColor[2] = ColorRGBA::getBlue(icol) / 255.0f;
	grad->m_InnerColor[3] = ColorRGBA::getAlpha(icol) / 255.0f;
	grad->m_OuterColor[0] = ColorRGBA::getRed(ocol) / 255.0f;
	grad->m_OuterColor[1] = ColorRGBA::getGreen(ocol) / 255.0f;
	grad->m_OuterColor[2] = ColorRGBA::getBlue(ocol) / 255.0f;
	grad->m_OuterColor[3] = ColorRGBA::getAlpha(ocol) / 255.0f;

	return handle;
}

ImagePatternHandle BGFXVGRenderer::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	ImagePatternHandle handle = { m_Context->m_ImagePatternAlloc.alloc() };
	if (handle.idx == bx::HandleAlloc::invalid) {
		return handle;
	}

	State* state = m_Context->getState();

	const float cs = cosf(angle);
	const float sn = sinf(angle);

	float mtx[6];
	mtx[0] = cs;
	mtx[1] = sn;
	mtx[2] = -sn;
	mtx[3] = cs;
	mtx[4] = cx;
	mtx[5] = cy;

	float patternMatrix[6];
	multiplyMatrix3(state->m_TransformMtx, mtx, patternMatrix);

	float inversePatternMatrix[6];
	invertMatrix3(patternMatrix, inversePatternMatrix);

	inversePatternMatrix[0] /= w;
	inversePatternMatrix[1] /= h;
	inversePatternMatrix[2] /= w;
	inversePatternMatrix[3] /= h;
	inversePatternMatrix[4] /= w;
	inversePatternMatrix[5] /= h;

	vg::ImagePattern* pattern = &m_Context->m_ImagePatterns[handle.idx];
	memcpy(pattern->m_Matrix, inversePatternMatrix, sizeof(float) * 6);
	pattern->m_ImageHandle = image;
	pattern->m_Alpha = alpha;
	
	return handle;
}

ImageHandle BGFXVGRenderer::CreateImageRGBA(int w, int h, uint32_t imageFlags, const uint8_t* data)
{
	return m_Context->createImageRGBA(w, h, imageFlags, data);
}

void BGFXVGRenderer::UpdateImage(ImageHandle image, const uint8_t* data)
{
	int w, h;
	GetImageSize(image, &w, &h);
	m_Context->updateImage(image, 0, 0, w, h, data);
}

void BGFXVGRenderer::GetImageSize(ImageHandle image, int* w, int* h)
{
	m_Context->getImageSize(image, w, h);
}

void BGFXVGRenderer::DeleteImage(ImageHandle image)
{
	m_Context->deleteImage(image);
}

bool BGFXVGRenderer::IsImageHandleValid(ImageHandle image)
{
	if (!isValid(image)) {
		return false;
	}

	Image* tex = &m_Context->m_Images[image.idx];
	return bgfx::isValid(tex->m_bgfxHandle);
}

void BGFXVGRenderer::PushState()
{
	assert(m_Context->m_CurStateID < MAX_STATE_STACK_SIZE - 1);
	memcpy(&m_Context->m_StateStack[m_Context->m_CurStateID + 1], &m_Context->m_StateStack[m_Context->m_CurStateID], sizeof(State));
	++m_Context->m_CurStateID;
}

void BGFXVGRenderer::PopState()
{
	assert(m_Context->m_CurStateID > 0);
	--m_Context->m_CurStateID;
}

void BGFXVGRenderer::ResetScissor()
{
	State* state = m_Context->getState();
	state->m_ScissorRect[0] = state->m_ScissorRect[1] = 0.0f;
	state->m_ScissorRect[2] = (float)m_Context->m_WinWidth;
	state->m_ScissorRect[3] = (float)m_Context->m_WinHeight;
}

void BGFXVGRenderer::Scissor(float x, float y, float w, float h)
{
	State* state = m_Context->getState();
	Vec2 pos = transformPos2D(x, y, state->m_TransformMtx);
	Vec2 size = transformVec2D(w, h, state->m_TransformMtx);
	state->m_ScissorRect[0] = pos.x;
	state->m_ScissorRect[1] = pos.y;
	state->m_ScissorRect[2] = size.x;
	state->m_ScissorRect[3] = size.y;
}

bool BGFXVGRenderer::IntersectScissor(float x, float y, float w, float h)
{
	State* state = m_Context->getState();
	Vec2 pos = transformPos2D(x, y, state->m_TransformMtx);
	Vec2 size = transformVec2D(w, h, state->m_TransformMtx);

	const float* rect = state->m_ScissorRect;

	float minx = max2(pos.x, rect[0]);
	float miny = max2(pos.y, rect[1]);
	float maxx = min2(pos.x + size.x, rect[0] + rect[2]);
	float maxy = min2(pos.y + size.y, rect[1] + rect[3]);

	state->m_ScissorRect[0] = minx;
	state->m_ScissorRect[1] = miny;
	state->m_ScissorRect[2] = max2(0.0f, maxx - minx);
	state->m_ScissorRect[3] = max2(0.0f, maxy - miny);

	// Return false in case scissor rect is too small for bgfx to handle correctly
	// NOTE: This was originally required in NanoVG because the scissor rectangle is passed
	// to the fragment shader. In this case, we might use it to discard anything that will
	// be rendered after this call.
	return state->m_ScissorRect[2] >= 1.0f && state->m_ScissorRect[3] >= 1.0f;
}

void BGFXVGRenderer::LoadIdentity()
{
	State* state = m_Context->getState();
	state->m_TransformMtx[0] = 1.0f;
	state->m_TransformMtx[1] = 0.0f;
	state->m_TransformMtx[2] = 0.0f;
	state->m_TransformMtx[3] = 1.0f;
	state->m_TransformMtx[4] = 0.0f;
	state->m_TransformMtx[5] = 0.0f;
	
	state->m_FontScale = getFontScale(state->m_TransformMtx);
	state->m_AvgScale = getAverageScale(state->m_TransformMtx);
}

void BGFXVGRenderer::Scale(float x, float y)
{
	State* state = m_Context->getState();
	state->m_TransformMtx[0] = x * state->m_TransformMtx[0];
	state->m_TransformMtx[1] = x * state->m_TransformMtx[1];
	state->m_TransformMtx[2] = y * state->m_TransformMtx[2];
	state->m_TransformMtx[3] = y * state->m_TransformMtx[3];

	state->m_FontScale = getFontScale(state->m_TransformMtx);
	state->m_AvgScale = getAverageScale(state->m_TransformMtx);
}

void BGFXVGRenderer::Translate(float x, float y)
{
	State* state = m_Context->getState();
	state->m_TransformMtx[4] += state->m_TransformMtx[0] * x + state->m_TransformMtx[2] * y;
	state->m_TransformMtx[5] += state->m_TransformMtx[1] * x + state->m_TransformMtx[3] * y;

	state->m_FontScale = getFontScale(state->m_TransformMtx);
	state->m_AvgScale = getAverageScale(state->m_TransformMtx);
}

void BGFXVGRenderer::Rotate(float ang_rad)
{
	const float c = cosf(ang_rad);
	const float s = sinf(ang_rad);

	State* state = m_Context->getState();
	float mtx[6];
	mtx[0] = c * state->m_TransformMtx[0] + s * state->m_TransformMtx[2];
	mtx[1] = c * state->m_TransformMtx[1] + s * state->m_TransformMtx[3];
	mtx[2] = -s * state->m_TransformMtx[0] + c * state->m_TransformMtx[2];
	mtx[3] = -s * state->m_TransformMtx[1] + c * state->m_TransformMtx[3];
	mtx[4] = state->m_TransformMtx[4];
	mtx[5] = state->m_TransformMtx[5];
	memcpy(state->m_TransformMtx, mtx, sizeof(float) * 6);

	state->m_FontScale = getFontScale(state->m_TransformMtx);
	state->m_AvgScale = getAverageScale(state->m_TransformMtx);
}

void BGFXVGRenderer::SetGlobalAlpha(float alpha)
{
	State* state = m_Context->getState();
	state->m_GlobalAlpha = alpha;
}

void BGFXVGRenderer::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	State* state = m_Context->getState();
	float scale = state->m_FontScale * m_Context->m_DevicePixelRatio;
#if !BATCH_TRANSFORM
	float invscale = 1.0f / scale;
	const float* scissor = state->m_ScissorRect;
#endif

	if (!end) {
		end = text + strlen(text);
	}

	if (end == text) {
		return;
	}

	FONScontext* fons = m_Context->m_FontStashContext;
	fonsSetSize(fons, font.m_Size * scale); // TODO: Check font size and discard call if smaller than threshold
	fonsSetAlign(fons, alignment);
	fonsSetFont(fons, font.m_Handle.idx);

	// Assume ASCII where each byte is a codepoint. Otherwise, the number 
	// of quads will be less than the number of chars/bytes.
	const uint32_t maxChars = (uint32_t)(end - text);
	if (m_Context->m_TextQuadCapacity < maxChars) {
		m_Context->m_TextQuadCapacity = maxChars;
		m_Context->m_TextQuads = (FONSquad*)BX_ALIGNED_REALLOC(m_Context->m_Allocator, m_Context->m_TextQuads, sizeof(FONSquad) * m_Context->m_TextQuadCapacity, 16);
	}
	
	FONStextIter iter;
	fonsTextIterInit(fons, &iter, x * scale, y * scale, text, end);

#if 0
	float textBounds[4] = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
#endif

	FONSquad* nextQuad = m_Context->m_TextQuads;
	FONStextIter prevIter = iter;
	while (fonsTextIterNext(fons, &iter, nextQuad)) {
		if (iter.prevGlyphIndex == -1) {
			// Draw all quads up to this point (if any) with the current atlas texture.
			const uint32_t numQuads = (uint32_t)(nextQuad - m_Context->m_TextQuads);
			if (numQuads != 0) {
				assert(numQuads <= m_Context->m_TextQuadCapacity);

#if 0
				// Check if the bounding rect of the text so far is inside the current scissor rect.
				if (textBounds[2] < scissor[0] ||
					textBounds[3] < scissor[1] ||
					textBounds[0] > (scissor[0] + scissor[2]) ||
					textBounds[1] > (scissor[1] + scissor[3])) {
					continue;
				}
#endif

				m_Context->renderTextQuads(m_Context->m_TextQuads, numQuads, color);

				// Reset next quad ptr.
				nextQuad = m_Context->m_TextQuads;

#if 0
				// Reset text bounds
				textBounds[0] = textBounds[1] = FLT_MAX;
				textBounds[2] = textBounds[3] = -FLT_MAX;
#endif
			}

			// Allocate a new atlas
			if (!m_Context->allocTextAtlas()) {
				break;
			}

			// And try fitting the glyph once again.
			iter = prevIter;
			fonsTextIterNext(fons, &iter, nextQuad);
			if (iter.prevGlyphIndex == -1) {
				break;
			}
		}

#if !BATCH_TRANSFORM
		// Transform quad...
		// TODO: Batch transform quads? Should handle culling correctly.
		Vec2 pMin = transformPos2D(nextQuad->x0 * invscale, nextQuad->y0 * invscale, state->m_TransformMtx);
		Vec2 pMax = transformPos2D(nextQuad->x1 * invscale, nextQuad->y1 * invscale, state->m_TransformMtx);

		nextQuad->x0 = pMin.x;
		nextQuad->y0 = pMin.y;
		nextQuad->x1 = pMax.x;
		nextQuad->y1 = pMax.y;

#if 0
		textBounds[0] = min2(pMin.x, textBounds[0]);
		textBounds[1] = min2(pMin.y, textBounds[1]);
		textBounds[2] = max2(pMax.x, textBounds[2]);
		textBounds[3] = max2(pMax.y, textBounds[3]);
#endif
#endif

		++nextQuad;
		prevIter = iter;
	}

	const uint32_t numQuads = (uint32_t)(nextQuad - m_Context->m_TextQuads);
	if (numQuads == 0) {
		return;
	}

	assert(numQuads <= m_Context->m_TextQuadCapacity);

#if 0
	// Check if the text box is inside the current scissor rect.
	if (textBounds[2] < scissor[0] ||
		textBounds[3] < scissor[1] ||
		textBounds[0] > (scissor[0] + scissor[2]) ||
		textBounds[1] > (scissor[1] + scissor[3])) {
		// Text isn't visible. Ignore command.
		return;
	}
#endif

	m_Context->renderTextQuads(m_Context->m_TextQuads, numQuads, color);
}

void BGFXVGRenderer::TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end)
{
	TextRow rows[2];
	int nrows = 0, i;
	int haling = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	int valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);
	float lineh = 0;

	lineh = GetTextLineHeight(font, alignment);

	alignment = FONS_ALIGN_LEFT | valign;

	while ((nrows = TextBreakLines(font, alignment, text, end, breakWidth, rows, 2, 0))) {
		for (i = 0; i < nrows; i++) {
			TextRow* row = &rows[i];
			if (haling & FONS_ALIGN_LEFT) {
				Text(font, alignment, color, x, y, row->start, row->end);
			} else if (haling & FONS_ALIGN_CENTER) {
				Text(font, alignment, color, x + breakWidth * 0.5f - row->width * 0.5f, y, row->start, row->end);
			} else if (haling & FONS_ALIGN_RIGHT) {
				Text(font, alignment, color, x + breakWidth - row->width, y, row->start, row->end);
			}

			y += lineh; // Assume line height multiplier to be 1.0 (NanoVG allows the user to change it, but I don't use it).
		}

		text = rows[nrows - 1].next;
	}
}

float BGFXVGRenderer::CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds)
{
	// nvgTextBounds()
	State* state = m_Context->getState();
	float scale = state->m_FontScale * m_Context->m_DevicePixelRatio;
	float invscale = 1.0f / scale;
	float width;

	FONScontext* fons = m_Context->m_FontStashContext;
	fonsSetSize(fons, font.m_Size * scale);
	fonsSetAlign(fons, alignment);
	fonsSetFont(fons, font.m_Handle.idx);

	width = fonsTextBounds(fons, x * scale, y * scale, text, end, bounds);
	if (bounds != nullptr) {
		// Use line bounds for height.
		fonsLineBounds(fons, y * scale, &bounds[1], &bounds[3]);
		bounds[0] *= invscale;
		bounds[1] *= invscale;
		bounds[2] *= invscale;
		bounds[3] *= invscale;
	}

	return width * invscale;
}

void BGFXVGRenderer::CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	State* state = m_Context->getState();
	TextRow rows[2];
	float scale = state->m_FontScale * m_Context->m_DevicePixelRatio;
	float invscale = 1.0f / scale;
	int nrows = 0, i;
	int haling = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	int valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);
	float rminy = 0, rmaxy = 0;
	float minx, miny, maxx, maxy;

	FONScontext* fons = m_Context->m_FontStashContext;
	fonsSetSize(fons, font.m_Size * scale);
	fonsSetFont(fons, font.m_Handle.idx);

	float lineh;
	fonsVertMetrics(fons, nullptr, nullptr, &lineh);
	lineh *= invscale;

	alignment = FONS_ALIGN_LEFT | valign;
	fonsSetAlign(fons, alignment);

	minx = maxx = x;
	miny = maxy = y;

	fonsLineBounds(fons, 0, &rminy, &rmaxy);
	rminy *= invscale;
	rmaxy *= invscale;

	while ((nrows = TextBreakLines(font, alignment, text, end, breakWidth, rows, 2, flags))) {
		for (i = 0; i < nrows; i++) {
			TextRow* row = &rows[i];
			float rminx, rmaxx, dx = 0;
	
			// Horizontal bounds
			if (haling & FONS_ALIGN_LEFT) {
				dx = 0;
			} else if (haling & FONS_ALIGN_CENTER) {
				dx = breakWidth * 0.5f - row->width * 0.5f;
			} else if (haling & FONS_ALIGN_RIGHT) {
				dx = breakWidth - row->width;
			}

			rminx = x + row->minx + dx;
			rmaxx = x + row->maxx + dx;

			minx = min2(minx, rminx);
			maxx = max2(maxx, rmaxx);

			// Vertical bounds.
			miny = min2(miny, y + rminy);
			maxy = max2(maxy, y + rmaxy);

			y += lineh; // Assume line height multiplier of 1.0
		}

		text = rows[nrows - 1].next;
	}

	if (bounds != nullptr) {
		bounds[0] = minx;
		bounds[1] = miny;
		bounds[2] = maxx;
		bounds[3] = maxy;
	}
}

float BGFXVGRenderer::GetTextLineHeight(const Font& font, uint32_t alignment)
{
	State* state = m_Context->getState();
	float scale = state->m_FontScale * m_Context->m_DevicePixelRatio;
	float invscale = 1.0f / scale;

	FONScontext* fons = m_Context->m_FontStashContext;
	fonsSetSize(fons, font.m_Size * scale);
	fonsSetAlign(fons, alignment);
	fonsSetFont(fons, font.m_Handle.idx);

	float lineh;
	fonsVertMetrics(fons, nullptr, nullptr, &lineh);
	lineh *= invscale;

	return lineh;
}

int BGFXVGRenderer::TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	// nvgTextBreakLines()
#define CP_SPACE  0
#define CP_NEW_LINE  1
#define CP_CHAR 2

	State* state = m_Context->getState();
	float scale = state->m_FontScale * m_Context->m_DevicePixelRatio;
	float invscale = 1.0f / scale;
	FONStextIter iter, prevIter;
	FONSquad q;
	int nrows = 0;
	float rowStartX = 0;
	float rowWidth = 0;
	float rowMinX = 0;
	float rowMaxX = 0;
	const char* rowStart = nullptr;
	const char* rowEnd = nullptr;
	const char* wordStart = nullptr;
	float wordStartX = 0;
	float wordMinX = 0;
	const char* breakEnd = nullptr;
	float breakWidth = 0;
	float breakMaxX = 0;
	int type = CP_SPACE, ptype = CP_SPACE;
	unsigned int pcodepoint = 0;

	if (maxRows == 0) {
		return 0;
	}

	if (end == nullptr) {
		end = text + strlen(text);
	}

	if (text == end) {
		return 0;
	}

	FONScontext* fons = m_Context->m_FontStashContext;
	fonsSetSize(fons, font.m_Size * scale);
	fonsSetAlign(fons, alignment);
	fonsSetFont(fons, font.m_Handle.idx);

	breakRowWidth *= scale;

	fonsTextIterInit(fons, &iter, 0, 0, text, end);
	prevIter = iter;
	while (fonsTextIterNext(fons, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && m_Context->allocTextAtlas()) { 
			// can not retrieve glyph?
			iter = prevIter;
			fonsTextIterNext(fons, &iter, &q); // try again
		}

		prevIter = iter;
		switch (iter.codepoint) {
		case 9:			// \t
		case 11:		// \v
		case 12:		// \f
		case 0x00a0:	// NBSP
			type = CP_SPACE;
			break;
		case 32:		// space 
			// JD: Treat spaces as regular characters in order to be able to have pre and post spaces in an edit box.
			if (flags & TextBreakFlags::SpacesAsChars) {
				type = CP_CHAR;
			} else {
				type = CP_SPACE;
			}
			break;
		case 10:		// \n
			type = pcodepoint == 13 ? CP_SPACE : CP_NEW_LINE;
			break;
		case 13:		// \r
			type = pcodepoint == 10 ? CP_SPACE : CP_NEW_LINE;
			break;
		case 0x0085:	// NEL
			type = CP_NEW_LINE;
			break;
		default:
			type = CP_CHAR;
			break;
		}

		if (type == CP_NEW_LINE) {
			// Always handle new lines.
			rows[nrows].start = rowStart != nullptr ? rowStart : iter.str;
			rows[nrows].end = rowEnd != nullptr ? rowEnd : iter.str;
			rows[nrows].width = rowWidth * invscale;
			rows[nrows].minx = rowMinX * invscale;
			rows[nrows].maxx = rowMaxX * invscale;
			rows[nrows].next = iter.next;
			nrows++;
			if (nrows >= maxRows) {
				return nrows;
			}

			// Set null break point
			breakEnd = rowStart;
			breakWidth = 0.0;
			breakMaxX = 0.0;
			
			// Indicate to skip the white space at the beginning of the row.
			rowStart = nullptr;
			rowEnd = nullptr;
			rowWidth = 0;
			rowMinX = rowMaxX = 0;
		} else {
			if (rowStart == nullptr) {
				// Skip white space until the beginning of the line
				if (type == CP_CHAR) {
					// The current char is the row so far
					rowStartX = iter.x;
					rowStart = iter.str;
					rowEnd = iter.next;
					rowWidth = iter.nextx - rowStartX; // q.x1 - rowStartX;
					rowMinX = q.x0 - rowStartX;
					rowMaxX = q.x1 - rowStartX;
					wordStart = iter.str;
					wordStartX = iter.x;
					wordMinX = q.x0 - rowStartX;

					// Set null break point
					breakEnd = rowStart;
					breakWidth = 0.0;
					breakMaxX = 0.0;
				}
			} else {
				float nextWidth = iter.nextx - rowStartX;

				// track last non-white space character
				if (type == CP_CHAR) {
					rowEnd = iter.next;
					rowWidth = iter.nextx - rowStartX;
					rowMaxX = q.x1 - rowStartX;
				}

				// track last end of a word
				if (ptype == CP_CHAR && type == CP_SPACE) {
					breakEnd = iter.str;
					breakWidth = rowWidth;
					breakMaxX = rowMaxX;
				}

				// track last beginning of a word
				if (ptype == CP_SPACE && type == CP_CHAR) {
					wordStart = iter.str;
					wordStartX = iter.x;
					wordMinX = q.x0 - rowStartX;
				}

				// Break to new line when a character is beyond break width.
				if (type == CP_CHAR && nextWidth > breakRowWidth) {
					// The run length is too long, need to break to new line.
					if (breakEnd == rowStart) {
						// The current word is longer than the row length, just break it from here.
						rows[nrows].start = rowStart;
						rows[nrows].end = iter.str;
						rows[nrows].width = rowWidth * invscale;
						rows[nrows].minx = rowMinX * invscale;
						rows[nrows].maxx = rowMaxX * invscale;
						rows[nrows].next = iter.str;
						nrows++;
						if (nrows >= maxRows) {
							return nrows;
						}
						rowStartX = iter.x;
						rowStart = iter.str;
						rowEnd = iter.next;
						rowWidth = iter.nextx - rowStartX;
						rowMinX = q.x0 - rowStartX;
						rowMaxX = q.x1 - rowStartX;
						wordStart = iter.str;
						wordStartX = iter.x;
						wordMinX = q.x0 - rowStartX;
					} else {
						// Break the line from the end of the last word, and start new line from the beginning of the new.
						rows[nrows].start = rowStart;
						rows[nrows].end = breakEnd;
						rows[nrows].width = breakWidth * invscale;
						rows[nrows].minx = rowMinX * invscale;
						rows[nrows].maxx = breakMaxX * invscale;
						rows[nrows].next = wordStart;
						nrows++;
						if (nrows >= maxRows) {
							return nrows;
						}
						rowStartX = wordStartX;
						rowStart = wordStart;
						rowEnd = iter.next;
						rowWidth = iter.nextx - rowStartX;
						rowMinX = wordMinX;
						rowMaxX = q.x1 - rowStartX;
						// No change to the word start
					}
					// Set null break point
					breakEnd = rowStart;
					breakWidth = 0.0;
					breakMaxX = 0.0;
				}
			}
		}

		pcodepoint = iter.codepoint;
		ptype = type;
	}

	// Break the line from the end of the last word, and start new line from the beginning of the new.
	if (rowStart != nullptr) {
		rows[nrows].start = rowStart;
		rows[nrows].end = rowEnd;
		rows[nrows].width = rowWidth * invscale;
		rows[nrows].minx = rowMinX * invscale;
		rows[nrows].maxx = rowMaxX * invscale;
		rows[nrows].next = end;
		nrows++;
	}

	return nrows;

#undef CP_SPACE
#undef CP_NEW_LINE
#undef CP_CHAR
}

int BGFXVGRenderer::TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions)
{
	State* state = m_Context->getState();
	float scale = state->m_FontScale * m_Context->m_DevicePixelRatio;
	float invscale = 1.0f / scale;
	FONStextIter iter, prevIter;
	FONSquad q;
	int npos = 0;

	if (!end) {
		end = text + strlen(text);
	}

	if (text == end) {
		return 0;
	}

	FONScontext* fons = m_Context->m_FontStashContext;
	fonsSetSize(fons, font.m_Size * scale);
	fonsSetAlign(fons, alignment);
	fonsSetFont(fons, font.m_Handle.idx);

	fonsTextIterInit(fons, &iter, x * scale, y * scale, text, end);
	prevIter = iter;
	while (fonsTextIterNext(fons, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && m_Context->allocTextAtlas()) {
			iter = prevIter;
			fonsTextIterNext(fons, &iter, &q);
		}

		prevIter = iter;
		positions[npos].str = iter.str;
		positions[npos].x = iter.x * invscale;
		positions[npos].minx = min2(iter.x, q.x0) * invscale;
		positions[npos].maxx = max2(iter.nextx, q.x1) * invscale;
		npos++;
		if (npos >= maxPositions)
			break;
	}

	return npos;
}

FontHandle BGFXVGRenderer::LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	return m_Context->loadFontFromMemory(name, data, size);
}

Font BGFXVGRenderer::CreateFontWithSize(const char* name, float size)
{
	Font f;
	f.m_Handle.idx = (uint16_t)fonsGetFontByName(m_Context->m_FontStashContext, name);
	f.m_Size = size;
	return f;
}

void Context::getImageSize(ImageHandle image, int* w, int* h)
{
	if (!isValid(image)) {
		*w = -1;
		*h = -1;
		return;
	}

	Image* tex = &m_Images[image.idx];
	if (!bgfx::isValid(tex->m_bgfxHandle)) {
		*w = -1;
		*h = -1;
		return;
	}

	*w = tex->m_Width;
	*h = tex->m_Height;
}

bool Context::allocTextAtlas()
{
	flushTextAtlasTexture();

	if (m_FontImageID + 1 >= MAX_FONT_IMAGES) {
		return false;
	}

	// if next fontImage already have a texture
	int iw, ih;
	if (isValid(m_FontImages[m_FontImageID + 1])) {
		getImageSize(m_FontImages[m_FontImageID + 1], &iw, &ih);
	} else {
		// calculate the new font image size and create it.
		getImageSize(m_FontImages[m_FontImageID], &iw, &ih);
		assert(iw != -1 && ih != -1);

		if (iw > ih) {
			ih *= 2;
		} else {
			iw *= 2;
		}

		const int maxTextureSize = (int)bgfx::getCaps()->limits.maxTextureSize;
		if (iw > maxTextureSize || ih > maxTextureSize) {
			iw = ih = maxTextureSize;
		}

		m_FontImages[m_FontImageID + 1] = createImageRGBA(iw, ih, ImageFlags::Filter_Bilinear, nullptr);
	}

	++m_FontImageID;

	fonsResetAtlas(m_FontStashContext, iw, ih);

	return true;
}

inline Vec2* Context::allocPathVertices(uint32_t n)
{
	if (m_NumPathVertices + n > m_PathVertexCapacity) {
		m_PathVertexCapacity = max2(m_PathVertexCapacity + n, m_PathVertexCapacity != 0 ? (m_PathVertexCapacity * 3) >> 1 : 16);
		m_PathVertices = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_PathVertices, sizeof(Vec2) * m_PathVertexCapacity, 16);
#if BATCH_TRANSFORM
		m_TransformedPathVertices = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_TransformedPathVertices, sizeof(Vec2) * m_PathVertexCapacity, 16);
#endif
	}

	Vec2* p = &m_PathVertices[m_NumPathVertices];
	m_NumPathVertices += n;
	return p;
}

inline void Context::addPathVertex(const Vec2& p)
{
#if BATCH_TRANSFORM
	assert(!m_PathVerticesTransformed);
#endif

	SubPath* path = getSubPath();

	// Don't allow adding new vertices to a closed sub-path.
	assert(!path->m_IsClosed);

	if (path->m_NumVertices != 0) {
		Vec2 d = m_PathVertices[path->m_NumVertices - 1] - p;
		if (d.dot(d) < 1e-5f) {
			return;
		}
	}

	Vec2* v = allocPathVertices(1);
	*v = p;

	path->m_NumVertices++;
}

#if BATCH_TRANSFORM
void transformQuads(FONSquad* __restrict quads, uint32_t n, const float* __restrict mtx)
{
	const bx::simd128_t mtx0 = bx::simd_splat(mtx[0]);
	const bx::simd128_t mtx1 = bx::simd_splat(mtx[1]);
	const bx::simd128_t mtx2 = bx::simd_splat(mtx[2]);
	const bx::simd128_t mtx3 = bx::simd_splat(mtx[3]);
	const bx::simd128_t mtx4 = bx::simd_splat(mtx[4]);
	const bx::simd128_t mtx5 = bx::simd_splat(mtx[5]);

	const uint32_t iter = n >> 1; // 2 quads per iteration
	for (uint32_t i = 0; i < iter; ++i) {
		bx::simd128_t q0 = bx::simd_ld(quads);
		bx::simd128_t q1 = bx::simd_ld(quads + 1);

		bx::simd128_t src0246 = bx::simd_shuf_xyAB(bx::simd_swiz_xzxz(q0), bx::simd_swiz_xzxz(q1));
		bx::simd128_t src1357 = bx::simd_shuf_xyAB(bx::simd_swiz_ywyw(q0), bx::simd_swiz_ywyw(q1));

		bx::simd128_t dst0246 = bx::simd_add(bx::simd_add(bx::simd_mul(src0246, mtx0), bx::simd_mul(src1357, mtx2)), mtx4);
		bx::simd128_t dst1357 = bx::simd_add(bx::simd_add(bx::simd_mul(src0246, mtx1), bx::simd_mul(src1357, mtx3)), mtx5);

		bx::simd128_t dst0123 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(dst0246, dst1357));
		bx::simd128_t dst4567 = bx::simd_swiz_xzyw(bx::simd_shuf_zwCD(dst0246, dst1357));

		bx::simd_st(quads, dst0123);
		bx::simd_st(quads + 1, dst4567);

		quads += 2;
	}

	const uint32_t rem = n & 1;
	if (rem) {
		bx::simd128_t q0 = bx::simd_ld(quads);

		bx::simd128_t src0202 = bx::simd_swiz_xzxz(q0);
		bx::simd128_t src1313 = bx::simd_swiz_ywyw(q0);

		bx::simd128_t dst0202 = bx::simd_add(bx::simd_add(bx::simd_mul(src0202, mtx0), bx::simd_mul(src1313, mtx2)), mtx4);
		bx::simd128_t dst1313 = bx::simd_add(bx::simd_add(bx::simd_mul(src0202, mtx1), bx::simd_mul(src1313, mtx3)), mtx5);

		bx::simd128_t dst0123 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(dst0202, dst1313));

		bx::simd_st(quads, dst0123);
	}
}
#endif

void Context::renderTextQuads(FONSquad* quads, uint32_t numQuads, Color color)
{
#if BATCH_TRANSFORM
	State* state = getState();
	float scale = state->m_FontScale * m_DevicePixelRatio;
	float invscale = 1.0f / scale;

	float mtx[6];
	mtx[0] = state->m_TransformMtx[0] * invscale;
	mtx[1] = state->m_TransformMtx[1] * invscale;
	mtx[2] = state->m_TransformMtx[2] * invscale;
	mtx[3] = state->m_TransformMtx[3] * invscale;
	mtx[4] = state->m_TransformMtx[4];
	mtx[5] = state->m_TransformMtx[5];

	// TODO: Calculate bounding rect of the quads.
	transformQuads(quads, numQuads, mtx);
#endif

	const uint32_t numDrawVertices = numQuads * 4;
	const uint32_t numDrawIndices = numQuads * 6;

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[m_FontImageID]);

	DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
	uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

	uint32_t startVertexID = cmd->m_NumVertices;

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;

	const FONSquad* q = quads;
	while (numQuads-- > 0) {
		SET_DRAW_VERTEX(dstVertex, q->x0, q->y0, q->s0, q->t0, color); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, q->x1, q->y0, q->s1, q->t0, color); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, q->x1, q->y1, q->s1, q->t1, color); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, q->x0, q->y1, q->s0, q->t1, color); ++dstVertex;

		*dstIndex++ = (uint16_t)startVertexID; *dstIndex++ = (uint16_t)(startVertexID + 1); *dstIndex++ = (uint16_t)(startVertexID + 2);
		*dstIndex++ = (uint16_t)startVertexID; *dstIndex++ = (uint16_t)(startVertexID + 2); *dstIndex++ = (uint16_t)(startVertexID + 3);

		startVertexID += 4;
		++q;
	}
}

void Context::renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numPathVertices, Color color)
{
	const State* state = getState();

	const uint32_t c = ColorRGBA::setAlpha(color, (uint8_t)(state->m_GlobalAlpha * ColorRGBA::getAlpha(color)));
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	const Vec2 uv = getWhitePixelUV();

	uint32_t numTris = numPathVertices - 2;
	const uint32_t numDrawVertices = numPathVertices;
	const uint32_t numDrawIndices = numTris * 3; // N - 2 triangles in a N pt fan, 3 indices per triangle

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
	uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

	const uint32_t startVertexID = cmd->m_NumVertices;

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;

	while (numPathVertices-- > 0) {
		SET_DRAW_VERTEX(dstVertex, vtx->x, vtx->y, uv.x, uv.y, c);
		++dstVertex;
		++vtx;
	}

	uint32_t secondTriVertex = startVertexID + 1;
	while (numTris-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 1);
		++secondTriVertex;
	}
}

void Context::renderConvexPolygonAA(const Vec2* vtx, uint32_t numPathVertices, Color color)
{
	const State* state = getState();
	const float aa = m_FringeWidth * 0.5f;

	const uint32_t c = ColorRGBA::setAlpha(color, (uint8_t)(state->m_GlobalAlpha * ColorRGBA::getAlpha(color)));
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	const uint32_t c0 = ColorRGBA::setAlpha(color, 0);
	const Vec2 uv = getWhitePixelUV();

	const uint32_t numTris = 
		(numPathVertices - 2) + // Triangle fan
		(numPathVertices * 2); // AA fringes
	const uint32_t numDrawVertices = numPathVertices * 2; // original polygon point + AA fringe point.
	const uint32_t numDrawIndices = numTris * 3;

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
	uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

	const uint32_t startVertexID = cmd->m_NumVertices;

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;

	// TODO: Assumes CCW order (otherwise fringes are generated on the wrong side of the polygon)
#if BATCH_PATH_DIRECTIONS
	Vec2* dir = allocPathDirections(numPathVertices);
	calcPathDirectionsSSE(vtx, numPathVertices, dir, true);

	Vec2 d01 = dir[numPathVertices - 1];

	for (uint32_t iSegment = 0; iSegment < numPathVertices; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2 d12 = dir[iSegment];

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_aa = v * aa;

		SET_DRAW_VERTEX(dstVertex, p1.x - v_aa.x, p1.y - v_aa.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, p1.x + v_aa.x, p1.y + v_aa.y, uv.x, uv.y, c0); ++dstVertex;

		d01 = d12;
	}
#else
	Vec2 d01 = vtx[0] - vtx[numPathVertices - 1];
	vec2Normalize(&d01);

	for (uint32_t iSegment = 0; iSegment < numPathVertices; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		Vec2 d12 = p2 - p1;
		vec2Normalize(&d12);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_aa = v * aa;

		SET_DRAW_VERTEX(dstVertex, p1.x - v_aa.x, p1.y - v_aa.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, p1.x + v_aa.x, p1.y + v_aa.y, uv.x, uv.y, c0); ++dstVertex;

		d01 = d12;
	}
#endif

	// Generate the triangle fan (original polygon)
	const uint32_t numFanTris = numPathVertices - 2;
	uint32_t secondTriVertex = startVertexID + 2;
	for (uint32_t i = 0; i < numFanTris; ++i) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 2);
		secondTriVertex += 2;
	}

	// Generate the AA fringes
	uint32_t firstVertexID = startVertexID;
	for (uint32_t i = 0; i < numPathVertices - 1; ++i) {
		*dstIndex++ = (uint16_t)firstVertexID;
		*dstIndex++ = (uint16_t)(firstVertexID + 1);
		*dstIndex++ = (uint16_t)(firstVertexID + 3);
		*dstIndex++ = (uint16_t)firstVertexID;
		*dstIndex++ = (uint16_t)(firstVertexID + 3);
		*dstIndex++ = (uint16_t)(firstVertexID + 2);
		firstVertexID += 2;
	}
	
	// Last segment
	*dstIndex++ = (uint16_t)firstVertexID;
	*dstIndex++ = (uint16_t)(firstVertexID + 1);
	*dstIndex++ = (uint16_t)(startVertexID + 1);
	*dstIndex++ = (uint16_t)(firstVertexID);
	*dstIndex++ = (uint16_t)(startVertexID + 1);
	*dstIndex++ = (uint16_t)startVertexID;
}

void Context::renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numPathVertices, ImagePatternHandle handle)
{
	assert(isValid(handle));

	ImagePattern* img = &m_ImagePatterns[handle.idx];
	uint32_t c = ColorRGBA::fromFloat(1.0f, 1.0f, 1.0f, img->m_Alpha);
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	const float* mtx = img->m_Matrix;

	uint32_t numTris = numPathVertices - 2;
	uint32_t numDrawVertices = numPathVertices;
	uint32_t numDrawIndices = numTris * 3; // N - 2 triangles in a N pt fan, 3 indices per triangle

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, img->m_ImageHandle);

	DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
	uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

	const uint32_t startVertexID = cmd->m_NumVertices;

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;

	for (uint32_t i = 0; i < numPathVertices; ++i) {
		const Vec2& p = vtx[i];
		Vec2 uv = transformPos2D(p.x, p.y, mtx);

		SET_DRAW_VERTEX(dstVertex, p.x, p.y, uv.x, uv.y, c);
		++dstVertex;
	}

	uint32_t secondTriVertex = startVertexID + 1;
	while (numTris-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 1);
		++secondTriVertex;
	}
}

void Context::renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numPathVertices, GradientHandle handle)
{
	assert(isValid(handle));

	uint32_t numTris = numPathVertices - 2;
	uint32_t numDrawVertices = numPathVertices;
	uint32_t numDrawIndices = numTris * 3; // N - 2 triangles in a N pt fan, 3 indices per triangle

	DrawCommand* cmd = allocDrawCommand_ColorGradient(numDrawVertices, numDrawIndices, handle);

	DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
	uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

	const uint32_t startVertexID = cmd->m_NumVertices;
	
	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;

	while (numPathVertices-- > 0) {
		SET_DRAW_VERTEX(dstVertex, vtx->x, vtx->y, 0.0f, 0.0f, ColorRGBA::White);
		++dstVertex;
		++vtx;
	}

	uint32_t secondTriVertex = startVertexID + 1;
	while (numTris-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 1);
		++secondTriVertex;
	}
}

void Context::renderPathStrokeAA(const Vec2* vtx, uint32_t numPathVertices, bool isClosed, float thickness, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	const State* state = getState();

	const float avgScale = state->m_AvgScale;
	float strokeWidth = clamp(thickness * avgScale, 0.0f, 200.0f);
	float alphaScale = state->m_GlobalAlpha;
	if (strokeWidth < 1.0f) {
		// nvgStroke()
		float alpha = clamp(strokeWidth, 0.0f, 1.0f);
		alphaScale *= alpha * alpha;

		strokeWidth = 1.0f;
	}

	uint32_t c = ColorRGBA::setAlpha(color, (uint8_t)(alphaScale * ColorRGBA::getAlpha(color)));
	uint32_t c0 = ColorRGBA::setAlpha(color, 0);

	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	const Vec2 uv = getWhitePixelUV();

	const uint32_t numSegments = numPathVertices - (isClosed ? 0 : 1);
	
	if (strokeWidth > m_FringeWidth) {
		const float halfStrokeWidth = (strokeWidth - m_FringeWidth) * 0.5f;
		const float middlePointFactor = m_FringeWidth / halfStrokeWidth;
		const float sidePointFactor = 1.0f + middlePointFactor;

		// TODO: Assume lineJoin == Miter and lineCap != Round
		const uint32_t numDrawVertices = numPathVertices * 4; // 4 vertices per path vertex (left, right, aaLeft, aaRight)
		const uint32_t numDrawIndices = numSegments * 18; // 6 tris / segment

		DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

		DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
		uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

		const uint32_t startVertexID = cmd->m_NumVertices;

		cmd->m_NumVertices += numDrawVertices;
		cmd->m_NumIndices += numDrawIndices;

#if BATCH_PATH_DIRECTIONS
		Vec2* dir = allocPathDirections(numPathVertices);
		calcPathDirectionsSSE(vtx, numPathVertices, dir, isClosed);

		Vec2 d01;
		if (!isClosed) {
			// First segment of an open path
			const Vec2& p0 = vtx[0];

			d01 = dir[0];

			const Vec2 l01Scaled = d01.perpCCW() * halfStrokeWidth;

			Vec2 leftPoint = p0 + l01Scaled;
			Vec2 rightPoint = p0 - l01Scaled;
			if (lineCap == LineCap::Square) {
				leftPoint -= d01 * halfStrokeWidth;
				rightPoint -= d01 * halfStrokeWidth;
			} else if (lineCap == LineCap::Round) {
				// TODO: Even if lineCap == Round assume Butt.
			}

			const Vec2 pScaled = p0 * middlePointFactor;
			const Vec2 leftPointAA = leftPoint * sidePointFactor - pScaled;
			const Vec2 rightPointAA = rightPoint * sidePointFactor - pScaled;

			SET_DRAW_VERTEX(dstVertex, leftPointAA.x, leftPointAA.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPointAA.x, rightPointAA.y, uv.x, uv.y, c0); ++dstVertex;
		} else {
			d01 = dir[numPathVertices - 1];
		}

		// Generate draw vertices...
		const uint32_t firstSegmentID = isClosed ? 0 : 1;
		for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];

			const Vec2 d12 = dir[iSegment];

			const Vec2 v = calcExtrusionVector(d01, d12);

			const Vec2 v_hsw = v * halfStrokeWidth;
			const Vec2 v_hsw_aa = v * (halfStrokeWidth + m_FringeWidth);

			const Vec2 leftPointAA = p1 + v_hsw_aa;
			const Vec2 leftPoint = p1 + v_hsw;
			const Vec2 rightPoint = p1 - v_hsw;
			const Vec2 rightPointAA = p1 - v_hsw_aa;

			SET_DRAW_VERTEX(dstVertex, leftPointAA.x, leftPointAA.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPointAA.x, rightPointAA.y, uv.x, uv.y, c0); ++dstVertex;

			d01 = d12;
		}
#else 
		Vec2 d01;
		if (!isClosed) {
			// First segment of an open path
			const Vec2& p0 = vtx[0];
			const Vec2& p1 = vtx[1];

			d01 = p1 - p0;
			vec2Normalize(&d01);

			const Vec2 l01Scaled = d01.perpCCW() * halfStrokeWidth;

			Vec2 leftPoint = p0 + l01Scaled;
			Vec2 rightPoint = p0 - l01Scaled;
			if (lineCap == LineCap::Square) {
				leftPoint -= d01 * halfStrokeWidth;
				rightPoint -= d01 * halfStrokeWidth;
			} else if (lineCap == LineCap::Round) {
				// TODO: Even if lineCap == Round assume Butt.
			}

			const Vec2 pScaled = p0 * middlePointFactor;
			const Vec2 leftPointAA = leftPoint * sidePointFactor - pScaled;
			const Vec2 rightPointAA = rightPoint * sidePointFactor - pScaled;

			SET_DRAW_VERTEX(dstVertex, leftPointAA.x, leftPointAA.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPointAA.x, rightPointAA.y, uv.x, uv.y, c0); ++dstVertex;
		} else {
			d01 = vtx[0] - vtx[numPathVertices - 1];
			vec2Normalize(&d01);
		}

		// Generate draw vertices...
		const uint32_t firstSegmentID = isClosed ? 0 : 1;
		for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];
			const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

			Vec2 d12 = p2 - p1;
			vec2Normalize(&d12);

			const Vec2 v = calcExtrusionVector(d01, d12);

			const Vec2 v_hsw = v * halfStrokeWidth;
			const Vec2 v_hsw_aa = v * (halfStrokeWidth + m_FringeWidth);

			const Vec2 leftPointAA = p1 + v_hsw_aa;
			const Vec2 leftPoint = p1 + v_hsw;
			const Vec2 rightPoint = p1 - v_hsw;
			const Vec2 rightPointAA = p1 - v_hsw_aa;

			SET_DRAW_VERTEX(dstVertex, leftPointAA.x, leftPointAA.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPointAA.x, rightPointAA.y, uv.x, uv.y, c0); ++dstVertex;

			d01 = d12;
		}
#endif

		if (!isClosed) {
			const Vec2& p1 = vtx[numPathVertices - 1];
			Vec2 pScaled = p1 * middlePointFactor;

			Vec2 l01 = d01.perpCCW() * halfStrokeWidth;

			Vec2 leftPoint = p1 + l01;
			Vec2 rightPoint = p1 - l01;
			if (lineCap == LineCap::Square) {
				leftPoint += d01 * halfStrokeWidth;
				rightPoint += d01 * halfStrokeWidth;
			} else if (lineCap == LineCap::Round) {
				// TODO: Even if lineCap == Round assume Butt.
			}

			Vec2 leftPointAA = leftPoint * sidePointFactor - pScaled;
			Vec2 rightPointAA = rightPoint * sidePointFactor - pScaled;

			SET_DRAW_VERTEX(dstVertex, leftPointAA.x, leftPointAA.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPointAA.x, rightPointAA.y, uv.x, uv.y, c0); ++dstVertex;
		}

		// Generate indices...
		uint32_t firstVertexID = startVertexID;
		uint32_t lastSegmentID = numSegments - (isClosed ? 1 : 0);
		for (uint32_t iSegment = 0; iSegment < lastSegmentID; ++iSegment) {
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 5);
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 5);
			*dstIndex++ = (uint16_t)(firstVertexID + 4);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 6);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 6);
			*dstIndex++ = (uint16_t)(firstVertexID + 5);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 3);
			*dstIndex++ = (uint16_t)(firstVertexID + 7);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 7);
			*dstIndex++ = (uint16_t)(firstVertexID + 6);
			firstVertexID += 4;
		}

		if (isClosed) {
			// Last segment
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(startVertexID + 1);
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(startVertexID + 1);
			*dstIndex++ = (uint16_t)(startVertexID);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(startVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(startVertexID + 2);
			*dstIndex++ = (uint16_t)(startVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 3);
			*dstIndex++ = (uint16_t)(startVertexID + 3);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(startVertexID + 3);
			*dstIndex++ = (uint16_t)(startVertexID + 2);
		}
	} else {
		const float halfStrokeWidth = m_FringeWidth;

		// TODO: Assume lineJoin == Miter and lineCap != Round
		const uint32_t numDrawVertices = numPathVertices * 3; // 3 vertices per path vertex (center, aaLeft, aaRight)
		const uint32_t numDrawIndices = numSegments * 12; // 4 tris / segment

		DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

		DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
		uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

		const uint32_t startVertexID = cmd->m_NumVertices;

		cmd->m_NumVertices += numDrawVertices;
		cmd->m_NumIndices += numDrawIndices;

#if BATCH_PATH_DIRECTIONS
		Vec2* dir = allocPathDirections(numPathVertices);
		calcPathDirectionsSSE(vtx, numPathVertices, dir, isClosed);

		Vec2 d01;
		if (!isClosed) {
			// First segment of an open path
			const Vec2& p0 = vtx[0];

			d01 = dir[0];

			Vec2 l01Scaled = d01.perpCCW() * halfStrokeWidth;

			Vec2 leftPoint = p0 + l01Scaled;
			Vec2 rightPoint = p0 - l01Scaled;
			if (lineCap == LineCap::Square) {
				leftPoint -= d01 * halfStrokeWidth;
				rightPoint -= d01 * halfStrokeWidth;
			} else if (lineCap == LineCap::Round) {
				// TODO: Even if lineCap == Round assume Butt.
			}

			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, p0.x, p0.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c0); ++dstVertex;
		} else {
			d01 = dir[numPathVertices - 1];
		}

		// Generate draw vertices...
		const uint32_t firstSegmentID = isClosed ? 0 : 1;
		for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];

			const Vec2 d12 = dir[iSegment];

			const Vec2 v = calcExtrusionVector(d01, d12);

			const Vec2 v_hsw = v * halfStrokeWidth;

			const Vec2 leftPoint = p1 + v_hsw;
			const Vec2 rightPoint = p1 - v_hsw;

			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, p1.x, p1.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c0); ++dstVertex;

			d01 = d12;
		}
#else
		Vec2 d01;
		if (!isClosed) {
			// First segment of an open path
			const Vec2& p0 = vtx[0];
			const Vec2& p1 = vtx[1];

			d01 = p1 - p0;
			vec2Normalize(&d01);

			Vec2 l01Scaled = d01.perpCCW() * halfStrokeWidth;

			Vec2 leftPoint = p0 + l01Scaled;
			Vec2 rightPoint = p0 - l01Scaled;
			if (lineCap == LineCap::Square) {
				leftPoint -= d01 * halfStrokeWidth;
				rightPoint -= d01 * halfStrokeWidth;
			} else if (lineCap == LineCap::Round) {
				// TODO: Even if lineCap == Round assume Butt.
			}

			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, p0.x, p0.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c0); ++dstVertex;
		} else {
			d01 = vtx[0] - vtx[numPathVertices - 1];
			vec2Normalize(&d01);
		}

		// Generate draw vertices...
		const uint32_t firstSegmentID = isClosed ? 0 : 1;
		for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
			const Vec2& p1 = vtx[iSegment];
			const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

			Vec2 d12 = p2 - p1;
			vec2Normalize(&d12);

			const Vec2 v = calcExtrusionVector(d01, d12);

			const Vec2 v_hsw = v * halfStrokeWidth;

			const Vec2 leftPoint = p1 + v_hsw;
			const Vec2 rightPoint = p1 - v_hsw;

			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, p1.x, p1.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c0); ++dstVertex;

			d01 = d12;
		}
#endif

		if (!isClosed) {
			const Vec2& p1 = vtx[numPathVertices - 1];

			Vec2 l01 = d01.perpCCW() * halfStrokeWidth;

			Vec2 leftPoint = p1 + l01;
			Vec2 rightPoint = p1 - l01;
			if (lineCap == LineCap::Square) {
				leftPoint += d01 * halfStrokeWidth;
				rightPoint += d01 * halfStrokeWidth;
			} else if (lineCap == LineCap::Round) {
				// TODO: Even if lineCap == Round assume Butt.
			}

			SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c0); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, p1.x, p1.y, uv.x, uv.y, c); ++dstVertex;
			SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c0); ++dstVertex;
		}

		// Generate indices...
		uint32_t firstVertexID = startVertexID;
		uint32_t lastSegmentID = numSegments - (isClosed ? 1 : 0);
		for (uint32_t iSegment = 0; iSegment < lastSegmentID; ++iSegment) {
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 4);
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 4);
			*dstIndex++ = (uint16_t)(firstVertexID + 3);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 5);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 5);
			*dstIndex++ = (uint16_t)(firstVertexID + 4);
			
			firstVertexID += 3;
		}

		if (isClosed) {
			// Last segment
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(startVertexID + 1);
			*dstIndex++ = (uint16_t)firstVertexID;
			*dstIndex++ = (uint16_t)(startVertexID + 1);
			*dstIndex++ = (uint16_t)startVertexID;
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(firstVertexID + 2);
			*dstIndex++ = (uint16_t)(startVertexID + 2);
			*dstIndex++ = (uint16_t)(firstVertexID + 1);
			*dstIndex++ = (uint16_t)(startVertexID + 2);
			*dstIndex++ = (uint16_t)(startVertexID + 1);
		}
	}
}

void Context::renderPathStrokeNoAA(const Vec2* vtx, uint32_t numPathVertices, bool isClosed, float thickness, Color color, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	const State* state = getState();

	const float avgScale = state->m_AvgScale;
	float strokeWidth = clamp(thickness * avgScale, 0.0f, 200.0f);
	float alphaScale = state->m_GlobalAlpha;
	if (strokeWidth < 1.0f) {
		// nvgStroke()
		float alpha = clamp(strokeWidth, 0.0f, 1.0f);
		alphaScale *= alpha * alpha;
		strokeWidth = 1.0f;
	}

	uint8_t newAlpha = (uint8_t)(alphaScale * ColorRGBA::getAlpha(color));
	if (newAlpha == 0) {
		return;
	}

	const uint32_t c = ColorRGBA::setAlpha(color, newAlpha);
	const Vec2 uv = getWhitePixelUV();

	const uint32_t numSegments = numPathVertices - (isClosed ? 0 : 1);

	const float halfStrokeWidth = strokeWidth * 0.5f;

	// TODO: Assume lineJoin == Miter and lineCap != Round
	const uint32_t numDrawVertices = numPathVertices * 2; // 2 vertices per path vertex (left, right)
	const uint32_t numDrawIndices = numSegments * 6; // 2 tris / segment

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	DrawVertex* dstVertex = &m_VertexBuffers[cmd->m_VertexBufferID].m_Vertices[cmd->m_FirstVertexID + cmd->m_NumVertices];
	uint16_t* dstIndex = &cmd->m_IB->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

	const uint32_t startVertexID = cmd->m_NumVertices;

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;

#if BATCH_PATH_DIRECTIONS
	Vec2* dir = allocPathDirections(numPathVertices);
	calcPathDirectionsSSE(vtx, numPathVertices, dir, isClosed);

	Vec2 d01;
	if (!isClosed) {
		// First segment of an open path
		const Vec2& p0 = vtx[0];
		d01 = dir[0];

		Vec2 l01Scaled = d01.perpCCW() * halfStrokeWidth;

		Vec2 leftPoint = p0 + l01Scaled;
		Vec2 rightPoint = p0 - l01Scaled;
		if (lineCap == LineCap::Square) {
			leftPoint -= d01 * halfStrokeWidth;
			rightPoint -= d01 * halfStrokeWidth;
		} else if (lineCap == LineCap::Round) {
			// TODO: Even if lineCap == Round assume Butt.
		}

		SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
	} else {
		d01 = dir[numPathVertices - 1];
	}

	const uint32_t firstSegmentID = isClosed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];

		const Vec2 d12 = dir[iSegment];

		const Vec2 v = calcExtrusionVector(d01, d12);

		const Vec2 v_hsw = v * halfStrokeWidth;

		const Vec2 leftPoint = p1 + v_hsw;
		const Vec2 rightPoint = p1 - v_hsw;

		SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;

		d01 = d12;
	}
#else
	Vec2 d01;
	if (!isClosed) {
		// First segment of an open path
		const Vec2& p0 = vtx[0];
		const Vec2& p1 = vtx[1];

		d01 = p1 - p0;
		vec2Normalize(&d01);

		Vec2 l01Scaled = d01.perpCCW() * halfStrokeWidth;

		Vec2 leftPoint = p0 + l01Scaled;
		Vec2 rightPoint = p0 - l01Scaled;
		if (lineCap == LineCap::Square) {
			leftPoint -= d01 * halfStrokeWidth;
			rightPoint -= d01 * halfStrokeWidth;
		} else if (lineCap == LineCap::Round) {
			// TODO: Even if lineCap == Round assume Butt.
		}

		SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
	} else {
		d01 = vtx[0] - vtx[numPathVertices - 1];
		vec2Normalize(&d01);
	}

	const uint32_t firstSegmentID = isClosed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		Vec2 d12 = p2 - p1;
		vec2Normalize(&d12);

		const Vec2 v = calcExtrusionVector(d01, d12);

		const Vec2 v_hsw = v * halfStrokeWidth;

		const Vec2 leftPoint = p1 + v_hsw;
		const Vec2 rightPoint = p1 - v_hsw;

		SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;

		d01 = d12;
	}
#endif

	if (!isClosed) {
		const Vec2& p1 = vtx[numPathVertices - 1];

		Vec2 l01 = d01.perpCCW() * halfStrokeWidth;

		Vec2 leftPoint = p1 + l01;
		Vec2 rightPoint = p1 - l01;
		if (lineCap == LineCap::Square) {
			leftPoint += d01 * halfStrokeWidth;
			rightPoint += d01 * halfStrokeWidth;
		} else if (lineCap == LineCap::Round) {
			// TODO: Even if lineCap == Round assume Butt.
		}

		SET_DRAW_VERTEX(dstVertex, leftPoint.x, leftPoint.y, uv.x, uv.y, c); ++dstVertex;
		SET_DRAW_VERTEX(dstVertex, rightPoint.x, rightPoint.y, uv.x, uv.y, c); ++dstVertex;
	}

	// Generate indices...
	uint32_t firstVertexID = startVertexID;
	uint32_t lastSegmentID = numSegments - (isClosed ? 1 : 0);
	for (uint32_t iSegment = 0; iSegment < lastSegmentID; ++iSegment) {
		*dstIndex++ = (uint16_t)firstVertexID;
		*dstIndex++ = (uint16_t)(firstVertexID + 1);
		*dstIndex++ = (uint16_t)(firstVertexID + 3);
		*dstIndex++ = (uint16_t)firstVertexID;
		*dstIndex++ = (uint16_t)(firstVertexID + 3);
		*dstIndex++ = (uint16_t)(firstVertexID + 2);
		firstVertexID += 2;
	}

	if (isClosed) {
		// Last segment
		*dstIndex++ = (uint16_t)firstVertexID;
		*dstIndex++ = (uint16_t)(firstVertexID + 1);
		*dstIndex++ = (uint16_t)(startVertexID + 1);
		*dstIndex++ = (uint16_t)firstVertexID;
		*dstIndex++ = (uint16_t)(startVertexID + 1);
		*dstIndex++ = (uint16_t)startVertexID;
	}
}

DrawCommand* Context::allocDrawCommand_TexturedVertexColor(uint32_t numVertices, uint32_t numIndices, ImageHandle img)
{
	assert(isValid(img));
	State* state = getState();
	const float* scissor = state->m_ScissorRect;

	VertexBuffer* vb = getVertexBuffer();
	if (vb->m_Count + numVertices > MAX_VB_VERTICES) {
		vb = allocVertexBuffer();

		// The currently active vertex buffer has changed so force a new draw command.
		m_ForceNewDrawCommand = true;
	}

	uint32_t vbID = (uint32_t)(vb - m_VertexBuffers);

	IndexBuffer* ib = getIndexBuffer();
	if (ib->m_Count + numIndices > ib->m_Capacity) {
		const uint32_t nextCapacity = ib->m_Capacity != 0 ? (ib->m_Capacity * 3) / 2 : 32;
		ib->m_Capacity = max2(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity);
	}

	if (!m_ForceNewDrawCommand && m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &m_DrawCommands[m_NumDrawCommands - 1];
		assert(prevCmd->m_VertexBufferID == vbID);
		assert(prevCmd->m_IB == ib);
		if (prevCmd->m_Type == DrawCommand::Type_TexturedVertexColor && 
			prevCmd->m_ImageHandle.idx == img.idx && 
			prevCmd->m_ScissorRect[0] == scissor[0] && 
			prevCmd->m_ScissorRect[1] == scissor[1] && 
			prevCmd->m_ScissorRect[2] == scissor[2] && 
			prevCmd->m_ScissorRect[3] == scissor[3]) {
			vb->m_Count += numVertices;
			ib->m_Count += numIndices;
			return prevCmd;
		}
	}
	
	// If we land here it means that the current draw command cannot be batched with the previous command.
	// Create a new one.
	if (m_NumDrawCommands + 1 >= m_DrawCommandCapacity) {
		m_DrawCommandCapacity = m_DrawCommandCapacity + 32;
		m_DrawCommands = (DrawCommand*)BX_REALLOC(m_Allocator, m_DrawCommands, sizeof(DrawCommand) * m_DrawCommandCapacity);
	}

	DrawCommand* cmd = &m_DrawCommands[m_NumDrawCommands++];
	cmd->m_VertexBufferID = vbID;
	cmd->m_FirstVertexID = vb->m_Count;
	cmd->m_IB = ib;
	cmd->m_FirstIndexID = ib->m_Count;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type_TexturedVertexColor;
	cmd->m_ImageHandle = img;
	cmd->m_GradientHandle = BGFX_INVALID_HANDLE;
	memcpy(cmd->m_ScissorRect, scissor, sizeof(float) * 4);

	vb->m_Count += numVertices;
	ib->m_Count += numIndices;

	m_ForceNewDrawCommand = false;

	return cmd;
}

DrawCommand* Context::allocDrawCommand_ColorGradient(uint32_t numVertices, uint32_t numIndices, GradientHandle gradientHandle)
{
	assert(isValid(gradientHandle));

	State* state = getState();
	const float* scissor = state->m_ScissorRect;

	VertexBuffer* vb = getVertexBuffer();
	if (vb->m_Count + numVertices > MAX_VB_VERTICES) {
		vb = allocVertexBuffer();

		// The currently active vertex buffer has changed so force a new draw command.
		m_ForceNewDrawCommand = true;
	}

	uint32_t vbID = (uint32_t)(vb - m_VertexBuffers);

	IndexBuffer* ib = getIndexBuffer();
	if (ib->m_Count + numIndices > ib->m_Capacity) {
		const uint32_t nextCapacity = ib->m_Capacity != 0 ? (ib->m_Capacity * 3) / 2 : 32;
		ib->m_Capacity = max2(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity);
	}

	if (!m_ForceNewDrawCommand) {
		DrawCommand* prevCmd = m_NumDrawCommands != 0 ? &m_DrawCommands[m_NumDrawCommands - 1] : nullptr;
		assert(prevCmd->m_VertexBufferID == vbID);
		assert(prevCmd->m_IB == ib);
		if (prevCmd->m_Type == DrawCommand::Type_ColorGradient &&
			prevCmd->m_GradientHandle.idx == gradientHandle.idx &&
			prevCmd->m_ScissorRect[0] == scissor[0] &&
			prevCmd->m_ScissorRect[1] == scissor[1] &&
			prevCmd->m_ScissorRect[2] == scissor[2] &&
			prevCmd->m_ScissorRect[3] == scissor[3]) {
			vb->m_Count += numVertices;
			ib->m_Count += numIndices;
			return prevCmd;
		}
	}

	// If we land here it means that the current draw command cannot be batched with the previous command.
	// Create a new one.
	if (m_NumDrawCommands + 1 >= m_DrawCommandCapacity) {
		m_DrawCommandCapacity = m_DrawCommandCapacity + 32;
		m_DrawCommands = (DrawCommand*)BX_REALLOC(m_Allocator, m_DrawCommands, sizeof(DrawCommand) * m_DrawCommandCapacity);
	}

	DrawCommand* cmd = &m_DrawCommands[m_NumDrawCommands++];
	cmd->m_VertexBufferID = vbID;
	cmd->m_FirstVertexID = vb->m_Count;
	cmd->m_IB = ib;
	cmd->m_FirstIndexID = ib->m_Count;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type_ColorGradient;
	cmd->m_ImageHandle = BGFX_INVALID_HANDLE;
	cmd->m_GradientHandle = gradientHandle;
	memcpy(cmd->m_ScissorRect, scissor, sizeof(float) * 4);

	vb->m_Count += numVertices;
	ib->m_Count += numIndices;

	m_ForceNewDrawCommand = false;

	return cmd;
}

VertexBuffer* Context::allocVertexBuffer()
{
	if (m_NumVertexBuffers + 1 > m_VertexBufferCapacity) {
		m_VertexBufferCapacity++;
		m_VertexBuffers = (VertexBuffer*)BX_REALLOC(m_Allocator, m_VertexBuffers, sizeof(VertexBuffer) * m_VertexBufferCapacity);
		
		m_VertexBuffers[m_VertexBufferCapacity - 1].m_bgfxHandle = BGFX_INVALID_HANDLE;
	}

	VertexBuffer* vb = &m_VertexBuffers[m_NumVertexBuffers++];
	vb->m_Vertices = (DrawVertex*)allocVertexBufferData();
	vb->m_Count = 0;

	return vb;
}

ImageHandle Context::createImageRGBA(uint32_t w, uint32_t h, uint32_t flags, const uint8_t* data)
{
	ImageHandle handle = allocImage();
	if (!isValid(handle)) {
		return handle;
	}

	Image* tex = &m_Images[handle.idx];
	tex->m_Width = (uint16_t)w;
	tex->m_Height = (uint16_t)h;

	uint32_t bgfxFlags = BGFX_TEXTURE_NONE;
	if (flags & ImageFlags::Filter_NearestUV) {
		bgfxFlags |= BGFX_TEXTURE_MIN_POINT | BGFX_TEXTURE_MAG_POINT;
	}
	if (flags & ImageFlags::Filter_NearestW) {
		bgfxFlags |= BGFX_TEXTURE_MIP_POINT;
	}
	tex->m_Flags = bgfxFlags;

	tex->m_bgfxHandle = bgfx::createTexture2D(tex->m_Width, tex->m_Height, false, 1, bgfx::TextureFormat::RGBA8, bgfxFlags);

	if (bgfx::isValid(tex->m_bgfxHandle) && data) {
		const uint32_t bytesPerPixel = 4;
		const uint32_t pitch = tex->m_Width * bytesPerPixel;
		const bgfx::Memory* mem = bgfx::copy(data, tex->m_Height * pitch);

		bgfx::updateTexture2D(tex->m_bgfxHandle, 0, 0, 0, 0, tex->m_Width, tex->m_Height, mem);
	}

	return handle;
}

ImageHandle Context::allocImage()
{
	ImageHandle handle = { m_ImageAlloc.alloc() };
	if (!isValid(handle)) {
		return handle;
	}

	if (handle.idx >= m_ImageCapacity) {
		int oldCapacity = m_ImageCapacity;

		m_ImageCapacity = max2<uint32_t>(m_ImageCapacity + 4, handle.idx + 1);
		m_Images = (Image*)BX_REALLOC(m_Allocator, m_Images, sizeof(Image) * m_ImageCapacity);
		if (!m_Images) {
			return BGFX_INVALID_HANDLE;
		}

		// Reset all new textures...
		for (uint32_t i = oldCapacity; i < m_ImageCapacity; ++i) {
			m_Images[i].reset();
		}
	}

	assert(handle.idx < m_ImageCapacity);
	Image* tex = &m_Images[handle.idx];
	assert(!bgfx::isValid(tex->m_bgfxHandle));
	tex->reset();
	return handle;
}

FontHandle Context::loadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	if (m_NextFontID == MAX_FONTS) {
		return BGFX_INVALID_HANDLE;
	}

	uint8_t* fontData = (uint8_t*)BX_ALLOC(m_Allocator, size);
	memcpy(fontData, data, size);

	// Don't let FontStash free the font data because it uses the global allocator 
	// which might be different than m_Allocator.
	int fonsHandle = fonsAddFontMem(m_FontStashContext, name, fontData, size, 0);
	if (fonsHandle == FONS_INVALID) {
		BX_FREE(m_Allocator, fontData);
		return BGFX_INVALID_HANDLE;
	}

	m_FontData[m_NextFontID++] = fontData;

	return { (uint16_t)fonsHandle };
}

void convertA8_to_RGBA8(uint32_t* rgba, const uint8_t* a8, uint32_t w, uint32_t h, uint32_t rgbColor)
{
	const uint32_t rgb0 = rgbColor & 0x00FFFFFF;

	int numPixels = w * h;
	for (int i = 0; i < numPixels; ++i) {
		*rgba++ = rgb0 | (((uint32_t) * a8) << 24);
		++a8;
	}
}

void Context::flushTextAtlasTexture()
{
	int dirty[4];
	if (!fonsValidateTexture(m_FontStashContext, dirty)) {
		return;
	}

	ImageHandle fontImage = m_FontImages[m_FontImageID];

	// Update texture
	if (!isValid(fontImage)) {
		return;
	}
	
	int iw, ih;
	const uint8_t* a8Data = fonsGetTextureData(m_FontStashContext, &iw, &ih);
	assert(iw > 0 && ih > 0);

	// TODO: Convert only the dirty part of the texture (it's the only part that will be uploaded to the backend)
	uint32_t* rgbaData = (uint32_t*)BX_ALLOC(m_Allocator, sizeof(uint32_t) * iw * ih);
	convertA8_to_RGBA8(rgbaData, a8Data, (uint32_t)iw, (uint32_t)ih, 0x00FFFFFF);

	int x = dirty[0];
	int y = dirty[1];
	int w = dirty[2] - dirty[0];
	int h = dirty[3] - dirty[1];
	updateImage(fontImage, x, y, w, h, (const uint8_t*)rgbaData);

	BX_FREE(m_Allocator, rgbaData);
}

bool Context::updateImage(ImageHandle image, int x, int y, int w, int h, const uint8_t* data)
{
	if (!isValid(image)) {
		return false;
	}

	Image* tex = &m_Images[image.idx];
	assert(bgfx::isValid(tex->m_bgfxHandle));

	const uint32_t bytesPerPixel = 4;
	const uint32_t pitch = tex->m_Width * bytesPerPixel;
	const bgfx::Memory* mem = bgfx::copy(data + y * pitch + x * bytesPerPixel, h * pitch);
	bgfx::updateTexture2D(tex->m_bgfxHandle, 0, 0, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, mem, (uint16_t)pitch);

	return true;
}

bool Context::deleteImage(ImageHandle img)
{
	if (!isValid(img)) {
		return false;
	}

	Image* tex = &m_Images[img.idx];
	assert(bgfx::isValid(tex->m_bgfxHandle));
	bgfx::destroyTexture(tex->m_bgfxHandle);
	tex->reset();
	
	m_ImageAlloc.free(img.idx);

	return true;
}

DrawVertex* Context::allocVertexBufferData()
{
	for (uint32_t i = 0; i < m_VBDataPoolCapacity; ++i) {
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)m_VBDataPool[i]) & 1) {
			// Remove the free flag
			m_VBDataPool[i] = (DrawVertex*)((uintptr_t)m_VBDataPool[i] & ~1);
			return m_VBDataPool[i];
		} else if (m_VBDataPool[i] == nullptr) {
			m_VBDataPool[i] = (DrawVertex*)BX_ALLOC(m_Allocator, sizeof(DrawVertex) * MAX_VB_VERTICES);
			return m_VBDataPool[i];
		}
	}

	uint32_t oldCapacity = m_VBDataPoolCapacity;
	m_VBDataPoolCapacity += 8;
	m_VBDataPool = (DrawVertex**)BX_REALLOC(m_Allocator, m_VBDataPool, sizeof(DrawVertex*) * m_VBDataPoolCapacity);
	memset(&m_VBDataPool[oldCapacity], 0, sizeof(DrawVertex*) * (m_VBDataPoolCapacity - oldCapacity));

	m_VBDataPool[oldCapacity] = (DrawVertex*)BX_ALLOC(m_Allocator, sizeof(DrawVertex) * MAX_VB_VERTICES);
	return m_VBDataPool[oldCapacity];
}

void Context::releaseVertexBufferData(DrawVertex* data)
{
	assert(data != nullptr);
	for (uint32_t i = 0; i < m_VBDataPoolCapacity; ++i) {
		if (m_VBDataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			m_VBDataPool[i] = (DrawVertex*)((uintptr_t)m_VBDataPool[i] | 1);
			return;
		}
	}
}

// Concave polygon decomposition
// https://mpen.ca/406/bayazit (seems to be down atm)
inline float area(const Vec2& a, const Vec2& b, const Vec2& c)
{
	return (((b.x - a.x) * (c.y - a.y)) - ((c.x - a.x) * (b.y - a.y)));
}

inline bool right(const Vec2& a, const Vec2& b, const Vec2& c)
{
	return area(a, b, c) > 0;
}

inline bool rightOn(const Vec2& a, const Vec2& b, const Vec2& c)
{
	return area(a, b, c) >= 0;
}

inline bool left(const Vec2& a, const Vec2& b, const Vec2& c)
{
	return area(a, b, c) < 0;
}

inline bool leftOn(const Vec2& a, const Vec2& b, const Vec2& c)
{
	return area(a, b, c) <= 0;
}

inline bool isReflex(const Vec2* points, uint32_t n, uint32_t i)
{
	if (i == 0) {
		return right(points[n - 1], points[0], points[1]);
	} else if (i == n - 1) {
		return right(points[n - 2], points[n - 1], points[0]);
	}

	return right(points[i - 1], points[i], points[i + 1]);
}

inline float sqdist(const Vec2& a, const Vec2& b)
{
	float dx = b.x - a.x;
	float dy = b.y - a.y;
	return dx * dx + dy * dy;
}

inline Vec2 intersection(const Vec2& p1, const Vec2& p2, const Vec2& q1, const Vec2& q2)
{
	Vec2 i;
	float a1, b1, c1, a2, b2, c2, det;
	a1 = p2.y - p1.y;
	b1 = p1.x - p2.x;
	c1 = a1 * p1.x + b1 * p1.y;
	a2 = q2.y - q1.y;
	b2 = q1.x - q2.x;
	c2 = a2 * q1.x + b2 * q1.y;
	det = a1 * b2 - a2 * b1;
	if (det != 0.0f) { // lines are not parallel
		i.x = (b2 * c1 - b1 * c2) / det;
		i.y = (a1 * c2 - a2 * c1) / det;
	}
	return i;
}

Vec2* Context::allocTempPoints(uint32_t n)
{
	// TODO: Pool?
	return (Vec2*)BX_ALLOC(m_Allocator, sizeof(Vec2) * n);
}

void Context::freeTempPoints(Vec2* ptr)
{
	BX_FREE(m_Allocator, ptr);
}

void Context::decomposeAndFillConcavePolygon(const Vec2* points, uint32_t numPoints, Color col, bool aa)
{
	assert(numPoints >= 3);

	Vec2 upperInt, lowerInt, p, closestVert;
	float upperDist, lowerDist, d, closestDist;
	uint32_t upperIndex = ~0u, lowerIndex = ~0u, closestIndex = ~0u;

	for (uint32_t i = 0; i < numPoints; ++i) {
		uint32_t prevPoint = i == 0 ? numPoints - 1 : i - 1;
		uint32_t nextPoint = i == numPoints - 1 ? 0 : i + 1;

		if (isReflex(points, numPoints, i)) {
			upperDist = lowerDist = FLT_MAX;

			for (uint32_t j = 0; j < numPoints; ++j) {
				uint32_t prevJ = j == 0 ? numPoints - 1 : j - 1;
				uint32_t nextJ = j == numPoints - 1 ? 0 : j + 1;

				// if line intersects with an edge
				if (left(points[prevPoint], points[i], points[j]) && rightOn(points[prevPoint], points[i], points[prevJ])) {
					// find the point of intersection
					p = intersection(points[prevPoint], points[i], points[j], points[prevJ]);

					// make sure it's inside the poly
					if (right(points[nextPoint], points[i], p)) {
						d = sqdist(points[i], p);

						if (d < lowerDist) { 
							// keep only the closest intersection
							lowerDist = d;
							lowerInt = p;
							lowerIndex = j;
						}
					}
				}

				if (left(points[nextPoint], points[i], points[nextJ]) && rightOn(points[nextPoint], points[i], points[j])) {
					p = intersection(points[nextPoint], points[i], points[j], points[nextJ]);

					if (left(points[prevPoint], points[i], p)) {
						d = sqdist(points[i], p);

						if (d < upperDist) {
							upperDist = d;
							upperInt = p;
							upperIndex = j;
						}
					}
				}
			}

			// if there are no vertices to connect to, choose a point in the middle
			assert(lowerIndex != ~0u);
			assert(upperIndex != ~0u);

			Vec2* lowerPoints = nullptr;
			Vec2* upperPoints = nullptr;
			uint32_t numLowerPoints = 0;
			uint32_t numUpperPoints = 0;

			if (lowerIndex == (upperIndex + 1) % numPoints) {
				p.x = (lowerInt.x + upperInt.x) * 0.5f;
				p.y = (lowerInt.y + upperInt.y) * 0.5f;

				if (i < upperIndex) {
					numLowerPoints = upperIndex + 1 - i;
					numUpperPoints = 1 + (lowerIndex != 0 ? (numPoints - lowerIndex) : 0) + (i + 1);

					lowerPoints = allocTempPoints(numLowerPoints + numUpperPoints);
					upperPoints = &lowerPoints[numLowerPoints];

					// Copy lower points...
					{
						uint32_t n = (upperIndex + 1 - i);
						memcpy(lowerPoints, &points[i], sizeof(Vec2) * n);
						lowerPoints[n] = p;
					}

					// Copy upper points
					{
						Vec2* writePtr = upperPoints;
						*writePtr++ = p;
						if (lowerIndex != 0) {
							uint32_t n = numPoints - lowerIndex;
							memcpy(writePtr, &points[lowerIndex], sizeof(Vec2) * n);
							writePtr += n;
						}
						memcpy(writePtr, &points[0], sizeof(Vec2) * (i + 1));
					}
				} else {
					numLowerPoints = ((i != 0) ? (numPoints - i) : 0) + (upperIndex + 1) + 1;
					numUpperPoints = 1 + (i + 1 - lowerIndex);

					lowerPoints = allocTempPoints(numLowerPoints + numUpperPoints);
					upperPoints = &lowerPoints[numLowerPoints];

					// Copy lower points
					{
						Vec2* writePtr = lowerPoints;
						if (i != 0) {
							uint32_t n = numPoints - i;
							memcpy(writePtr, &points[i], sizeof(Vec2) * n);
							writePtr += n;
						}

						memcpy(writePtr, &points[0], sizeof(Vec2) * (upperIndex + 1));
						writePtr[upperIndex + 1] = p;
					}

					// Copy upper points
					{
						upperPoints[0] = p;
						memcpy(&upperPoints[1], &points[lowerIndex], sizeof(Vec2) * (i + 1 - lowerIndex));
					}
				}
			} else {
				// connect to the closest point within the triangle
				if (lowerIndex > upperIndex) {
					upperIndex += numPoints;
				}

				closestDist = FLT_MAX;
				for (uint32_t j = lowerIndex; j <= upperIndex; ++j) {
					if (leftOn(points[prevPoint], points[i], points[j]) && rightOn(points[nextPoint], points[i], points[j])) {
						d = sqdist(points[i], points[j]);

						if (d < closestDist) {
							closestDist = d;
							closestVert = points[j];
							closestIndex = j % numPoints;
						}
					}
				}

				assert(closestIndex != ~0u);
				if (i < closestIndex) {
					numLowerPoints = closestIndex + 1 - i;
					numUpperPoints = (closestIndex != 0 ? (numPoints - closestIndex) : 0) + (i + 1);

					lowerPoints = allocTempPoints(numLowerPoints + numUpperPoints);
					upperPoints = &lowerPoints[numLowerPoints];

					// Copy lower points
					memcpy(lowerPoints, &points[i], sizeof(Vec2) * (closestIndex + 1 - i));

					// Copy upper points
					{
						Vec2* writePtr = upperPoints;
						if (closestIndex != 0) {
							memcpy(writePtr, &points[closestIndex], sizeof(Vec2) * (numPoints - closestIndex));
							writePtr += (numPoints - closestIndex);
						}

						memcpy(writePtr, &points[0], sizeof(Vec2) * (i + 1));
					}
				} else {
					numLowerPoints = (i != 0 ? (numPoints - i) : 0) + (closestIndex + 1);
					numUpperPoints = i + 1 - closestIndex;

					lowerPoints = allocTempPoints(numLowerPoints + numUpperPoints);
					upperPoints = &lowerPoints[numLowerPoints];

					// Copy lower points
					{
						Vec2* writePtr = lowerPoints;
						if (i != 0) {
							memcpy(writePtr, &points[i], sizeof(Vec2) * (numPoints - i));
							writePtr += (numPoints - i);
						}

						memcpy(writePtr, &points[0], sizeof(Vec2) * (closestIndex + 1));
					}

					// Copy upper points
					memcpy(upperPoints, &points[closestIndex], sizeof(Vec2) * (i + 1 - closestIndex));
				}
			}

			// solve smallest poly first
			if (numLowerPoints < numUpperPoints) {
				decomposeAndFillConcavePolygon(lowerPoints, numLowerPoints, col, aa);
				decomposeAndFillConcavePolygon(upperPoints, numUpperPoints, col, aa);
			} else {
				decomposeAndFillConcavePolygon(upperPoints, numUpperPoints, col, aa);
				decomposeAndFillConcavePolygon(lowerPoints, numLowerPoints, col, aa);
			}

			freeTempPoints(lowerPoints);

			return;
		}
	}

	if (aa) {
		renderConvexPolygonAA(points, numPoints, col);
	} else {
		renderConvexPolygonNoAA(points, numPoints, col);
	}
}
}
