// TODO:
// 4) Layers
// 5) Check polygon winding and force CCW order because otherwise AA fringes are generated inside the polygon!

#if VG_CONFIG_DEBUG
#define BX_TRACE(_format, ...) \
	do { \
		bx::debugPrintf(BX_FILE_LINE_LITERAL "BGFXVGRenderer " _format "\n", ##__VA_ARGS__); \
	} while(0)

#define BX_WARN(_condition, _format, ...) \
	do { \
		if (!(_condition) ) { \
			BX_TRACE(BX_FILE_LINE_LITERAL _format, ##__VA_ARGS__); \
		} \
	} while(0)

#define BX_CHECK(_condition, _format, ...) \
	do { \
		if (!(_condition) ) { \
			BX_TRACE(BX_FILE_LINE_LITERAL _format, ##__VA_ARGS__); \
			bx::debugBreak(); \
		} \
	} while(0)
#endif

#include <bx/bx.h>
#include <bx/debug.h>

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4127) // conditional expression is constant (e.g. BezierTo)
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4706) // assignment withing conditional expression
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wshadow");

#include "bgfxvg_renderer.h"
#include "shape.h"
#include "path.h"

//#define FONTSTASH_IMPLEMENTATION 
#include "../nanovg/fontstash.h"

#include <bgfx/embedded_shader.h>
#include <bx/allocator.h>
#include <bx/math.h>
#include <bx/mutex.h>
#include <bx/simd_t.h>
#include <bx/handlealloc.h>
#include <memory.h>
#include <math.h>
#include <float.h>
#include "util.h"

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
#define MAX_TEXTURES             64
#define MAX_FONT_IMAGES          4
#define MIN_FONT_ATLAS_SIZE      512

#if !defined(FONS_QUAD_SIMD) || !FONS_QUAD_SIMD
#error "FONS_QUAD_SIMD should be set to 1"
#endif

#if !FONS_CUSTOM_WHITE_RECT
#pragma message("FONS_CUSTOM_WHITE_RECT should be set to 1 to avoid UV problems with cached shapes")
#endif

static const bgfx::EmbeddedShader s_EmbeddedShaders[] =
{
	BGFX_EMBEDDED_SHADER(vs_solid_color),
	BGFX_EMBEDDED_SHADER(fs_solid_color),
	BGFX_EMBEDDED_SHADER(vs_gradient),
	BGFX_EMBEDDED_SHADER(fs_gradient),

	BGFX_EMBEDDED_SHADER_END()
};

#define CMD_READ(type, buffer) *(type*)buffer; buffer += sizeof(type);

struct State
{
	float m_TransformMtx[6];
	float m_ScissorRect[4];
	float m_GlobalAlpha;
	float m_FontScale;
	float m_AvgScale;
};

struct VertexBuffer
{
	Vec2* m_Pos;
	Vec2* m_UV;
	uint32_t* m_Color;
	uint32_t m_Count;
	bgfx::DynamicVertexBufferHandle m_PosBufferHandle;
	bgfx::DynamicVertexBufferHandle m_UVBufferHandle;
	bgfx::DynamicVertexBufferHandle m_ColorBufferHandle;
};

struct IndexBuffer
{
	uint16_t* m_Indices;
	uint32_t m_Count;
	uint32_t m_Capacity;
	bgfx::DynamicIndexBufferHandle m_bgfxHandle;

	IndexBuffer() : m_Indices(nullptr), m_Count(0), m_Capacity(0), m_bgfxHandle(VG_INVALID_HANDLE)
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
	uint16_t m_Width;
	uint16_t m_Height;
	uint32_t m_Flags;
	bgfx::TextureHandle m_bgfxHandle;

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

	Type m_Type;
	uint32_t m_VertexBufferID;
	uint32_t m_FirstVertexID;
	uint32_t m_FirstIndexID;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
	uint16_t m_ScissorRect[4];
	uint16_t m_HandleID; // Type_TexturedVertexColor => ImageHandle, Type_ColorGradient => GradientHandle
};

struct CachedDrawCommand
{
	Vec2* m_Pos;
	Vec2* m_UV;
	uint32_t* m_Color;
	uint16_t* m_Indices;
	uint16_t m_NumVertices;
	uint16_t m_NumIndices;
};

struct CachedTextCommand
{
	String* m_Text;
	uint32_t m_Alignment;
	Color m_Color;
	Vec2 m_Pos;
};

struct CachedShape
{
	CachedDrawCommand* m_DrawCommands;
	CachedTextCommand* m_StaticTextCommands;
	const uint8_t** m_TextCommands;
	uint32_t m_NumDrawCommands;
	uint32_t m_NumTextCommands;
	uint32_t m_NumStaticTextCommands;
	float m_InvTransformMtx[6];
	float m_AvgScale;

	CachedShape() 
		: m_AvgScale(0.0f)
		, m_DrawCommands(nullptr)
		, m_NumDrawCommands(0)
		, m_TextCommands(nullptr)
		, m_StaticTextCommands(nullptr)
		, m_NumTextCommands(0)
		, m_NumStaticTextCommands(0)
	{
		bx::memSet(m_InvTransformMtx, 0, sizeof(float) * 6);
		m_InvTransformMtx[0] = m_InvTransformMtx[3] = 1.0f;
	}

	~CachedShape()
	{}
};

struct Context
{
	bx::AllocatorI* m_Allocator;

	VertexBuffer* m_VertexBuffers;
	uint32_t m_NumVertexBuffers;
	uint32_t m_VertexBufferCapacity;

	Vec2** m_Vec2DataPool;
	uint32_t** m_Uint32DataPool;
	uint32_t m_Vec2DataPoolCapacity;
	uint32_t m_Uint32DataPoolCapacity;
#if BX_CONFIG_SUPPORTS_THREADING
	bx::Mutex m_DataPoolMutex;
#endif

	DrawCommand* m_DrawCommands;
	uint32_t m_NumDrawCommands;
	uint32_t m_DrawCommandCapacity;

	Image* m_Images;
	uint32_t m_ImageCapacity;
	bx::HandleAllocT<MAX_TEXTURES> m_ImageAlloc;

	Path* m_Path;
	Vec2* m_TransformedPathVertices;
	uint32_t m_NumTransformedPathVertices;
	bool m_PathVerticesTransformed;

	State m_StateStack[MAX_STATE_STACK_SIZE];
	uint32_t m_CurStateID;

	IndexBuffer* m_IndexBuffer;

	FONSquad* m_TextQuads;
	Vec2* m_TextVertices;
	uint32_t m_TextQuadCapacity;

	// Temporary geometry used when generating strokes with round joints.
	// This is needed because the number of vertices for each rounded corner depends
	// on the angle between the 2 connected line segments
	Vec2* m_TempGeomPos;
	uint32_t* m_TempGeomColor;
	uint16_t* m_TempGeomIndex;
	uint32_t m_TempGeomNumVertices;
	uint32_t m_TempGeomVertexCapacity;
	uint32_t m_TempGeomNumIndices;
	uint32_t m_TempGeomIndexCapacity;

	FONScontext* m_FontStashContext;
	FONSstring m_TextString;
	ImageHandle m_FontImages[MAX_FONT_IMAGES];
	int m_FontImageID;
	void* m_FontData[VG_CONFIG_MAX_FONTS];
	uint32_t m_NextFontID;

	Gradient m_Gradients[VG_CONFIG_MAX_GRADIENTS];
	uint32_t m_NextGradientID;

	ImagePattern m_ImagePatterns[VG_CONFIG_MAX_IMAGE_PATTERNS];
	uint32_t m_NextImagePatternID;

	bgfx::VertexDecl m_PosVertexDecl;
	bgfx::VertexDecl m_UVVertexDecl;
	bgfx::VertexDecl m_ColorVertexDecl;
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

	Context(bx::AllocatorI* allocator, uint8_t viewID);
	~Context();

	// Helpers...
	inline State* getState()               { return &m_StateStack[m_CurStateID]; }
	inline VertexBuffer* getVertexBuffer() { return &m_VertexBuffers[m_NumVertexBuffers - 1]; }
	inline IndexBuffer* getIndexBuffer()   { return m_IndexBuffer; }
	inline Vec2 getWhitePixelUV()
	{
		int w, h;
		getImageSize(m_FontImages[0], &w, &h);
		return Vec2(0.5f / (float)w, 0.5f / (float)h);
	}

	bool init();
	void beginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio);
	void endFrame();

	// Commands
	void beginPath();
	void moveTo(float x, float y);
	void lineTo(float x, float y);
	void bezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	void arcTo(float x1, float y1, float x2, float y2, float radius);
	void rect(float x, float y, float w, float h);
	void roundedRect(float x, float y, float w, float h, float r);
	void circle(float cx, float cy, float r);
	void polyline(const Vec2* coords, uint32_t numPoints);
	void closePath();
	void fillConvexPath(Color col, bool aa);
	void fillConvexPath(GradientHandle gradient, bool aa);
	void fillConvexPath(ImagePatternHandle img, bool aa);
	void fillConcavePath(Color col, bool aa);
	void strokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin);
	GradientHandle createLinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	GradientHandle createBoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
	GradientHandle createRadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	ImagePatternHandle createImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha);
	void text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end);
	void textBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end);
	float calcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds);
	void calcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags);
	float getTextLineHeight(const Font& font, uint32_t alignment);
	int textBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags);
	int textGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions);

	// State
	void pushState();
	void popState();
	void resetScissor();
	void setScissor(float x, float y, float w, float h);
	bool intersectScissor(float x, float y, float w, float h);
	void setGlobalAlpha(float alpha);
	void onTransformationMatrixUpdated();
	void transformIdentity();
	void transformScale(float x, float y);
	void transformTranslate(float x, float y);
	void transformRotate(float ang_rad);
	void transformMult(const float* mtx, bool pre);
	void transformPath();

	// Shapes
	Shape* createShape(uint32_t flags);
	void destroyShape(Shape* shape);
	void submitShape(Shape* shape, GetStringByIDFunc* stringCallback, void* userData);

	// Vertex buffers
	VertexBuffer* allocVertexBuffer();
	Vec2* allocVertexBufferData_Vec2();
	uint32_t* allocVertexBufferData_Uint32();
	void releaseVertexBufferData_Vec2(Vec2* data);
	void releaseVertexBufferData_Uint32(uint32_t* data);

	// Draw commands
	DrawCommand* allocDrawCommand_TexturedVertexColor(uint32_t numVertices, uint32_t numIndices, ImageHandle img);
	DrawCommand* allocDrawCommand_ColorGradient(uint32_t numVertices, uint32_t numIndices, GradientHandle gradient);

	// Concave polygon decomposition
	void decomposeAndFillConcavePolygon(const Vec2* points, uint32_t numPoints, Color col, bool aa);
	Vec2* allocTempPoints(uint32_t n);
	void freeTempPoints(Vec2* ptr);

	// Textures
	ImageHandle allocImage();
	ImageHandle createImageRGBA(uint16_t width, uint16_t height, uint32_t flags, const uint8_t* data);
	bool updateImage(ImageHandle img, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
	bool deleteImage(ImageHandle img);
	void getImageSize(ImageHandle image, int* w, int* h);
	bool isImageHandleValid(ImageHandle image);

	// Fonts
	FontHandle loadFontFromMemory(const char* name, const uint8_t* data, uint32_t size);
	bool allocTextAtlas();
	void flushTextAtlasTexture();

	// Rendering
	template<bool _Closed, LineCap::Enum lineCap, LineJoin::Enum lineJoin>
	void renderPathStrokeAA(const Vec2* vtx, uint32_t numVertices, float strokeWidth, Color color);
	template<LineCap::Enum lineCap, LineJoin::Enum lineJoin>
	void renderPathStrokeAAThin(const Vec2* vtx, uint32_t numVertices, Color color, bool closed);
	template<bool _Closed, LineCap::Enum lineCap, LineJoin::Enum lineJoin>
	void renderPathStrokeNoAA(const Vec2* vtx, uint32_t numVertices, float strokeWidth, Color color);

	void renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numVertices, Color color);
	void renderConvexPolygonAA(const Vec2* vtx, uint32_t numVertices, Color color);
	void renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numVertices, ImagePatternHandle img);
	void renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numVertices, GradientHandle grad);
	void renderTextQuads(uint32_t numQuads, Color color);

	// Caching
	void cachedShapeReset(CachedShape* shape);
	void cachedShapeAddDrawCommand(CachedShape* shape, const DrawCommand* cmd);
	void cachedShapeAddTextCommand(CachedShape* shape, const uint8_t* cmdData);
	void cachedShapeAddTextCommand(CachedShape* shape, String* str, uint32_t alignment, Color col, float x, float y);
	void cachedShapeRender(const CachedShape* shape, GetStringByIDFunc* stringCallback, void* userData);

	// Temporary geometry
	void tempGeomReset();
	void tempGeomExpandVB(uint32_t n);
	void tempGeomExpandIB(uint32_t n);
	// NOTE: The only reason there's a separate function for the actual reallocation is because MSVC 
	// doesn't inline the conditional if the reallocation happens inside tempGeomExpandXX() funcs. Since
	// temp pos/color buffers are reused all the time, reallocations will be rare. So it should be better
	// to have the conditional inlined.
	void tempGeomReallocVB(uint32_t n);
	void tempGeomReallocIB(uint32_t n);
	void tempGeomAddPos(const Vec2* srcPos, uint32_t n);
	void tempGeomAddPosColor(const Vec2* srcPos, const uint32_t* srcColor, uint32_t n);
	void tempGeomAddIndices(const uint16_t* __restrict src, uint32_t n);

	// Strings
	String* createString(const Font& font, const char* text, const char* end);
	void destroyString(String* str);
	void text(String* str, uint32_t alignment, Color color, float x, float y);
};

static void releaseVertexBufferDataCallback_Vec2(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseVertexBufferData_Vec2((Vec2*)ptr);
}

static void releaseVertexBufferDataCallback_Uint32(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseVertexBufferData_Uint32((uint32_t*)ptr);
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
		bx::memSet(inv, 0, sizeof(float) * 6);
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
	return bx::fmin(quantize(getAverageScale(xform), 0.01f), 4.0f);
}

inline void vec2Normalize(Vec2* v)
{
	float lenSqr = v->x * v->x + v->y * v->y;
	float invLen = lenSqr < 1e-5f ? 0.0f : (1.0f / sqrtf(lenSqr));

	v->x *= invLen;
	v->y *= invLen;
}

inline Vec2 calcExtrusionVector(const Vec2& d01, const Vec2& d12)
{
	// v is the vector from the path point to the outline point, assuming a stroke width of 1.0.
	// Equation obtained by solving the intersection of the 2 line segments. d01 and d12 are 
	// assumed to be normalized.
	Vec2 v = d01.perpCCW();
	const float cross = d12.cross(d01);
	if (bx::fabs(cross) > 1e-5f) {
		v = (d01 - d12) * (1.0f / cross);
	}

	return v;
}

void batchTransformPositions(const Vec2* __restrict v, uint32_t n, Vec2* __restrict p, const float* __restrict mtx);
void batchTransformPositions_Unaligned(const Vec2* __restrict v, uint32_t n, Vec2* __restrict p, const float* __restrict mtx);
void batchTransformDrawIndices(const uint16_t* __restrict src, uint32_t n, uint16_t* __restrict dst, uint16_t delta);
void batchTransformTextQuads(const FONSquad* __restrict quads, uint32_t n, const float* __restrict mtx, Vec2* __restrict transformedVertices);

//////////////////////////////////////////////////////////////////////////
// BGFXVGRenderer
//
BGFXVGRenderer::BGFXVGRenderer() : m_Context(nullptr)
{
}

BGFXVGRenderer::~BGFXVGRenderer()
{
	if (m_Context) {
		BX_DELETE(m_Context->m_Allocator, m_Context);
		m_Context = nullptr;
	}
}

bool BGFXVGRenderer::init(uint8_t viewID, bx::AllocatorI* allocator)
{
	m_Context = (Context*)BX_NEW(allocator, Context)(allocator, viewID);
	if (!m_Context) {
		return false;
	}

	if (!m_Context->init()) {
		BX_DELETE(allocator, m_Context);
		return false;
	}

	return true;
}

void BGFXVGRenderer::BeginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio)
{
	m_Context->beginFrame(windowWidth, windowHeight, devicePixelRatio);
}

void BGFXVGRenderer::EndFrame()
{
	m_Context->endFrame();
}

void BGFXVGRenderer::BeginPath()
{
	m_Context->beginPath();
}

void BGFXVGRenderer::MoveTo(float x, float y)
{
	m_Context->moveTo(x, y);
}

void BGFXVGRenderer::LineTo(float x, float y)
{
	m_Context->lineTo(x, y);
}

void BGFXVGRenderer::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	m_Context->bezierTo(c1x, c1y, c2x, c2y, x, y);
}

void BGFXVGRenderer::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	m_Context->arcTo(x1, y1, x2, y2, radius);
}

void BGFXVGRenderer::Rect(float x, float y, float w, float h)
{
	m_Context->rect(x, y, w, h);
}

void BGFXVGRenderer::RoundedRect(float x, float y, float w, float h, float r)
{
	m_Context->roundedRect(x, y, w, h, r);
}

void BGFXVGRenderer::Circle(float cx, float cy, float r)
{
	m_Context->circle(cx, cy, r);
}

void BGFXVGRenderer::Polyline(const float* coords, uint32_t numPoints)
{
	m_Context->polyline((const Vec2*)coords, numPoints);
}

void BGFXVGRenderer::ClosePath()
{
	m_Context->closePath();
}

void BGFXVGRenderer::FillConvexPath(Color col, bool aa)
{
#if VG_CONFIG_FORCE_AA_OFF
	aa = false;
#endif

	m_Context->fillConvexPath(col, aa);
}

void BGFXVGRenderer::FillConvexPath(GradientHandle gradient, bool aa)
{
#if VG_CONFIG_FORCE_AA_OFF
	aa = false;
#endif

	m_Context->fillConvexPath(gradient, aa);
}

void BGFXVGRenderer::FillConvexPath(ImagePatternHandle img, bool aa)
{
#if VG_CONFIG_FORCE_AA_OFF
	aa = false;
#endif

	m_Context->fillConvexPath(img, aa);
}

void BGFXVGRenderer::FillConcavePath(Color col, bool aa)
{
#if VG_CONFIG_FORCE_AA_OFF
	aa = false;
#endif
	
	m_Context->fillConcavePath(col, aa);
}

void BGFXVGRenderer::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
#if VG_CONFIG_FORCE_AA_OFF
	aa = false;
#endif

	m_Context->strokePath(col, width, aa, lineCap, lineJoin);
}

GradientHandle BGFXVGRenderer::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	return m_Context->createLinearGradient(sx, sy, ex, ey, icol, ocol);
}

GradientHandle BGFXVGRenderer::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	return m_Context->createBoxGradient(x, y, w, h, r, f, icol, ocol);
}

GradientHandle BGFXVGRenderer::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	return m_Context->createRadialGradient(cx, cy, inr, outr, icol, ocol);
}

ImagePatternHandle BGFXVGRenderer::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	return m_Context->createImagePattern(cx, cy, w, h, angle, image, alpha);
}

ImageHandle BGFXVGRenderer::CreateImageRGBA(uint16_t w, uint16_t h, uint32_t imageFlags, const uint8_t* data)
{
	return m_Context->createImageRGBA(w, h, imageFlags, data);
}

void BGFXVGRenderer::UpdateImage(ImageHandle image, const uint8_t* data)
{
	if (!isValid(image)) {
		return;
	}

	int w, h;
	m_Context->getImageSize(image, &w, &h);
	m_Context->updateImage(image, 0, 0, (uint16_t)w, (uint16_t)h, data);
}

void BGFXVGRenderer::UpdateSubImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data)
{
	m_Context->updateImage(image, x, y, w, h, data);
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
	return m_Context->isImageHandleValid(image);
}

void BGFXVGRenderer::PushState()
{
	m_Context->pushState();
}

void BGFXVGRenderer::PopState()
{
	m_Context->popState();
}

void BGFXVGRenderer::ResetScissor()
{
	m_Context->resetScissor();
}

void BGFXVGRenderer::Scissor(float x, float y, float w, float h)
{
	m_Context->setScissor(x, y, w, h);
}

bool BGFXVGRenderer::IntersectScissor(float x, float y, float w, float h)
{
	return m_Context->intersectScissor(x, y, w, h);
}

void BGFXVGRenderer::LoadIdentity()
{
	m_Context->transformIdentity();
}

void BGFXVGRenderer::Scale(float x, float y)
{
	m_Context->transformScale(x, y);
}

void BGFXVGRenderer::Translate(float x, float y)
{
	m_Context->transformTranslate(x, y);
}

void BGFXVGRenderer::Rotate(float ang_rad)
{
	m_Context->transformRotate(ang_rad);
}

void BGFXVGRenderer::ApplyTransform(const float* mtx, bool pre)
{
	m_Context->transformMult(mtx, pre);
}

void BGFXVGRenderer::SetGlobalAlpha(float alpha)
{
	m_Context->setGlobalAlpha(alpha);
}

void BGFXVGRenderer::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	m_Context->text(font, alignment, color, x, y, text, end);
}

void BGFXVGRenderer::TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end)
{
	m_Context->textBox(font, alignment, color, x, y, breakWidth, text, end);
}

float BGFXVGRenderer::CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds)
{
	return m_Context->calcTextBounds(font, alignment, x, y, text, end, bounds);
}

void BGFXVGRenderer::CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	m_Context->calcTextBoxBounds(font, alignment, x, y, breakWidth, text, end, bounds, flags);
}

float BGFXVGRenderer::GetTextLineHeight(const Font& font, uint32_t alignment)
{
	return m_Context->getTextLineHeight(font, alignment);
}

int BGFXVGRenderer::TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	return m_Context->textBreakLines(font, alignment, text, end, breakRowWidth, rows, maxRows, flags);
}

int BGFXVGRenderer::TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions)
{
	return m_Context->textGlyphPositions(font, alignment, x, y, text, end, positions, maxPositions);
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

Shape* BGFXVGRenderer::CreateShape(uint32_t flags)
{
	return m_Context->createShape(flags);
}

void BGFXVGRenderer::DestroyShape(Shape* shape)
{
	m_Context->destroyShape(shape);
}

void BGFXVGRenderer::SubmitShape(Shape* shape)
{
	m_Context->submitShape(shape, nullptr, nullptr);
}

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
void BGFXVGRenderer::SubmitShape(Shape* shape, GetStringByIDFunc stringCallback, void* userData)
{
	m_Context->submitShape(shape, &stringCallback, userData);
}
#endif // VG_CONFIG_SHAPE_DYNAMIC_TEXT

String* BGFXVGRenderer::CreateString(const char* fontName, float fontSize, const char* text, const char* end)
{
	return m_Context->createString(CreateFontWithSize(fontName, fontSize), text, end);
}

void BGFXVGRenderer::DestroyString(String* str)
{
	m_Context->destroyString(str);
}

void BGFXVGRenderer::Text(String* str, uint32_t alignment, Color color, float x, float y)
{
	m_Context->text(str, alignment, color, x, y);
}

//////////////////////////////////////////////////////////////////////////
// Context
//
Context::Context(bx::AllocatorI* allocator, uint8_t viewID) :
	m_Allocator(allocator),
	m_VertexBuffers(nullptr),
	m_NumVertexBuffers(0),
	m_VertexBufferCapacity(0),
	m_Vec2DataPool(nullptr),
	m_Uint32DataPool(nullptr),
	m_Vec2DataPoolCapacity(0),
	m_Uint32DataPoolCapacity(0),
	m_DrawCommands(nullptr),
	m_NumDrawCommands(0),
	m_DrawCommandCapacity(0),
	m_Images(nullptr),
	m_ImageCapacity(0),
	m_Path(nullptr),
	m_TransformedPathVertices(nullptr),
	m_NumTransformedPathVertices(0),
	m_PathVerticesTransformed(false),
	m_CurStateID(0),
	m_IndexBuffer(nullptr),
	m_TextQuads(nullptr),
	m_TextVertices(nullptr),
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
	m_NextFontID(0),
	m_TempGeomPos(nullptr),
	m_TempGeomColor(nullptr),
	m_TempGeomIndex(nullptr),
	m_TempGeomNumVertices(0),
	m_TempGeomVertexCapacity(0),
	m_TempGeomNumIndices(0),
	m_TempGeomIndexCapacity(0),
	m_NextGradientID(0),
	m_NextImagePatternID(0)
{
	for (uint32_t i = 0; i < MAX_FONT_IMAGES; ++i) {
		m_FontImages[i] = VG_INVALID_HANDLE;
	}

	for (uint32_t i = 0; i < DrawCommand::NumTypes; ++i) {
		m_ProgramHandle[i] = BGFX_INVALID_HANDLE;
	}

	m_TexUniform = BGFX_INVALID_HANDLE;
	m_PaintMatUniform = BGFX_INVALID_HANDLE;
	m_ExtentRadiusFeatherUniform = BGFX_INVALID_HANDLE;
	m_InnerColorUniform = BGFX_INVALID_HANDLE;
	m_OuterColorUniform = BGFX_INVALID_HANDLE;

	bx::memSet(m_FontData, 0, sizeof(void*) * VG_CONFIG_MAX_FONTS);
	bx::memSet(m_Gradients, 0, sizeof(Gradient) * VG_CONFIG_MAX_GRADIENTS);
	bx::memSet(m_ImagePatterns, 0, sizeof(ImagePattern) * VG_CONFIG_MAX_IMAGE_PATTERNS);
	bx::memSet(m_StateStack, 0, sizeof(State) * MAX_STATE_STACK_SIZE);

	fonsInitString(&m_TextString);
}

Context::~Context()
{
	fonsDestroyString(&m_TextString);

	for (uint32_t i = 0; i < DrawCommand::NumTypes; ++i) {
		if (bgfx::isValid(m_ProgramHandle[i])) {
			bgfx::destroy(m_ProgramHandle[i]);
		}
	}

	bgfx::destroy(m_TexUniform);
	bgfx::destroy(m_PaintMatUniform);
	bgfx::destroy(m_ExtentRadiusFeatherUniform);
	bgfx::destroy(m_InnerColorUniform);
	bgfx::destroy(m_OuterColorUniform);

	for (uint32_t i = 0; i < m_VertexBufferCapacity; ++i) {
		if (bgfx::isValid(m_VertexBuffers[i].m_PosBufferHandle)) {
			bgfx::destroy(m_VertexBuffers[i].m_PosBufferHandle);
		}
		if (bgfx::isValid(m_VertexBuffers[i].m_UVBufferHandle)) {
			bgfx::destroy(m_VertexBuffers[i].m_UVBufferHandle);
		}
		if (bgfx::isValid(m_VertexBuffers[i].m_ColorBufferHandle)) {
			bgfx::destroy(m_VertexBuffers[i].m_ColorBufferHandle);
		}
	}
	BX_FREE(m_Allocator, m_VertexBuffers);

	if (bgfx::isValid(m_IndexBuffer->m_bgfxHandle)) {
		bgfx::destroy(m_IndexBuffer->m_bgfxHandle);
	}
	BX_ALIGNED_FREE(m_Allocator, m_IndexBuffer->m_Indices, 16);
	BX_DELETE(m_Allocator, m_IndexBuffer);

	for (uint32_t i = 0; i < m_Vec2DataPoolCapacity; ++i) {
		Vec2* buffer = m_Vec2DataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (Vec2*)((uintptr_t)buffer & ~1);
			BX_ALIGNED_FREE(m_Allocator, buffer, 16);
		}
	}
	BX_FREE(m_Allocator, m_Vec2DataPool);
	m_Vec2DataPoolCapacity = 0;

	for (uint32_t i = 0; i < m_Uint32DataPoolCapacity; ++i) {
		uint32_t* buffer = m_Uint32DataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (uint32_t*)((uintptr_t)buffer & ~1);
			BX_ALIGNED_FREE(m_Allocator, buffer, 16);
		}
	}
	BX_FREE(m_Allocator, m_Uint32DataPool);
	m_Uint32DataPoolCapacity = 0;

	BX_FREE(m_Allocator, m_DrawCommands);

	// Manually delete font data
	for (int i = 0; i < VG_CONFIG_MAX_FONTS; ++i) {
		if (m_FontData[i]) {
			BX_FREE(m_Allocator, m_FontData[i]);
		}
	}

	fonsDeleteInternal(m_FontStashContext);

	for (uint32_t i = 0; i < MAX_FONT_IMAGES; ++i) {
		deleteImage(m_FontImages[i]);
	}

	BX_DELETE(m_Allocator, m_Path);
	m_Path = nullptr;

	BX_FREE(m_Allocator, m_Images);

	BX_ALIGNED_FREE(m_Allocator, m_TextQuads, 16);
	BX_ALIGNED_FREE(m_Allocator, m_TextVertices, 16);
	BX_ALIGNED_FREE(m_Allocator, m_TransformedPathVertices, 16);
	BX_ALIGNED_FREE(m_Allocator, m_TempGeomPos, 16);
	BX_ALIGNED_FREE(m_Allocator, m_TempGeomColor, 16);
	BX_ALIGNED_FREE(m_Allocator, m_TempGeomIndex, 16);
}

bool Context::init()
{
	m_DevicePixelRatio = 1.0f;
	m_TesselationTolerance = 0.25f;
	m_FringeWidth = 1.0f;
	m_CurStateID = 0;
	m_StateStack[0].m_GlobalAlpha = 1.0f;
	resetScissor();
	transformIdentity();

	m_Path = BX_NEW(m_Allocator, Path)(m_Allocator);
	m_IndexBuffer = BX_NEW(m_Allocator, IndexBuffer)();

	m_PosVertexDecl.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float).end();
	m_UVVertexDecl.begin().add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
	m_ColorVertexDecl.begin().add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();

	bgfx::RendererType::Enum bgfxRendererType = bgfx::getRendererType();
	m_ProgramHandle[DrawCommand::Type_TexturedVertexColor] =
		bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_solid_color"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_solid_color"),
		true);

	m_ProgramHandle[DrawCommand::Type_ColorGradient] =
		bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_gradient"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_gradient"),
		true);

	m_TexUniform = bgfx::createUniform("s_tex", bgfx::UniformType::Int1, 1);
	m_PaintMatUniform = bgfx::createUniform("u_paintMat", bgfx::UniformType::Mat3, 1);
	m_ExtentRadiusFeatherUniform = bgfx::createUniform("u_extentRadiusFeather", bgfx::UniformType::Vec4, 1);
	m_InnerColorUniform = bgfx::createUniform("u_innerCol", bgfx::UniformType::Vec4, 1);
	m_OuterColorUniform = bgfx::createUniform("u_outerCol", bgfx::UniformType::Vec4, 1);

	// Init font stash
	const bgfx::Caps* caps = bgfx::getCaps();
	FONSparams fontParams;
	bx::memSet(&fontParams, 0, sizeof(fontParams));
	fontParams.width = MIN_FONT_ATLAS_SIZE;
	fontParams.height = MIN_FONT_ATLAS_SIZE;
	fontParams.flags = FONS_ZERO_TOPLEFT;
#if FONS_CUSTOM_WHITE_RECT
	// NOTE: White rect might get too large but since the atlas limit is the texture size limit
	// it should be that large. Otherwise shapes cached when the atlas was 512x512 will get wrong
	// white pixel UVs when the atlas gets to the texture size limit (should not happen but better
	// be safe).
	fontParams.whiteRectWidth = caps->limits.maxTextureSize / MIN_FONT_ATLAS_SIZE;
	fontParams.whiteRectHeight = caps->limits.maxTextureSize / MIN_FONT_ATLAS_SIZE;
#endif
	fontParams.renderCreate = nullptr;
	fontParams.renderUpdate = nullptr;
	fontParams.renderDraw = nullptr;
	fontParams.renderDelete = nullptr;
	fontParams.userPtr = nullptr;
	m_FontStashContext = fonsCreateInternal(&fontParams);
	if (!m_FontStashContext) {
		return false;
	}

	m_FontImages[0] = createImageRGBA((uint16_t)fontParams.width, (uint16_t)fontParams.height, ImageFlags::Filter_Bilinear, nullptr);
	if (!isValid(m_FontImages[0])) {
		return false;
	}
	m_FontImageID = 0;

	return true;
}

void Context::beginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio)
{
	BX_CHECK(windowWidth > 0 && windowWidth < 65536, "Invalid window width");
	BX_CHECK(windowHeight > 0 && windowHeight < 65536, "Invalid window height");
	m_WinWidth = (uint16_t)windowWidth;
	m_WinHeight = (uint16_t)windowHeight;
	m_DevicePixelRatio = devicePixelRatio;
	m_TesselationTolerance = 0.25f / devicePixelRatio;
	m_FringeWidth = 1.0f / devicePixelRatio;

	BX_CHECK(m_CurStateID == 0, "State stack hasn't been properly reset in the previous frame");
	resetScissor();
	transformIdentity();

	m_NumVertexBuffers = 0;
	allocVertexBuffer();

	m_IndexBuffer->reset();
	m_NumDrawCommands = 0;
	m_ForceNewDrawCommand = true;

	m_NextGradientID = 0;
	m_NextImagePatternID = 0;
}

void Context::endFrame()
{
	BX_CHECK(m_CurStateID == 0, "PushState()/PopState() mismatch");
	if (m_NumDrawCommands == 0) {
		// Release the vertex buffer allocated in beginFrame()
		releaseVertexBufferData_Vec2(m_VertexBuffers[0].m_Pos);
		releaseVertexBufferData_Vec2(m_VertexBuffers[0].m_UV);
		releaseVertexBufferData_Uint32(m_VertexBuffers[0].m_Color);

		return;
	}

	flushTextAtlasTexture();

	// Update bgfx vertex buffers...
	for (uint32_t iVB = 0; iVB < m_NumVertexBuffers; ++iVB) {
		VertexBuffer* vb = &m_VertexBuffers[iVB];

		if (!bgfx::isValid(vb->m_PosBufferHandle)) {
			vb->m_PosBufferHandle = bgfx::createDynamicVertexBuffer(MAX_VB_VERTICES, m_PosVertexDecl, 0);
		}
		if (!bgfx::isValid(vb->m_UVBufferHandle)) {
			vb->m_UVBufferHandle = bgfx::createDynamicVertexBuffer(MAX_VB_VERTICES, m_UVVertexDecl, 0);
		}
		if (!bgfx::isValid(vb->m_ColorBufferHandle)) {
			vb->m_ColorBufferHandle = bgfx::createDynamicVertexBuffer(MAX_VB_VERTICES, m_ColorVertexDecl, 0);
		}

		const bgfx::Memory* posMem = bgfx::makeRef(vb->m_Pos, sizeof(Vec2) * vb->m_Count, releaseVertexBufferDataCallback_Vec2, this);
		const bgfx::Memory* uvMem = bgfx::makeRef(vb->m_UV, sizeof(Vec2) * vb->m_Count, releaseVertexBufferDataCallback_Vec2, this);
		const bgfx::Memory* colorMem = bgfx::makeRef(vb->m_Color, sizeof(uint32_t) * vb->m_Count, releaseVertexBufferDataCallback_Uint32, this);

		bgfx::updateDynamicVertexBuffer(vb->m_PosBufferHandle, 0, posMem);
		bgfx::updateDynamicVertexBuffer(vb->m_UVBufferHandle, 0, uvMem);
		bgfx::updateDynamicVertexBuffer(vb->m_ColorBufferHandle, 0, colorMem);

		vb->m_Pos = nullptr;
		vb->m_UV = nullptr;
		vb->m_Color = nullptr;
	}

	// Update bgfx index buffer...
	const bgfx::Memory* indexMem = bgfx::copy(&m_IndexBuffer->m_Indices[0], sizeof(uint16_t) * m_IndexBuffer->m_Count);
	if (!bgfx::isValid(m_IndexBuffer->m_bgfxHandle)) {
		m_IndexBuffer->m_bgfxHandle = bgfx::createDynamicIndexBuffer(indexMem, BGFX_BUFFER_ALLOW_RESIZE);
	} else {
		bgfx::updateDynamicIndexBuffer(m_IndexBuffer->m_bgfxHandle, 0, indexMem);
	}
	
	float viewMtx[16];
	float projMtx[16];
	bx::mtxIdentity(viewMtx);
	bx::mtxOrtho(projMtx, 0.0f, m_WinWidth, m_WinHeight, 0.0f, 0.0f, 1.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(m_ViewID, viewMtx, projMtx);

	uint16_t prevScissorRect[4] = { 0, 0, m_WinWidth, m_WinHeight };
	uint16_t prevScissorID = UINT16_MAX;

	const uint32_t numCommands = m_NumDrawCommands;
	for (uint32_t iCmd = 0; iCmd < numCommands; ++iCmd) {
		DrawCommand* cmd = &m_DrawCommands[iCmd];

		VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
		bgfx::setVertexBuffer(0, vb->m_PosBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setVertexBuffer(1, vb->m_UVBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setVertexBuffer(2, vb->m_ColorBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setIndexBuffer(m_IndexBuffer->m_bgfxHandle, cmd->m_FirstIndexID, cmd->m_NumIndices);

		// Set scissor.
		{
			const uint16_t* cmdScissorRect = &cmd->m_ScissorRect[0];
			if (!bx::memCmp(cmdScissorRect, &prevScissorRect[0], sizeof(uint16_t) * 4)) {
				// Re-set the previous cached scissor rect (submit() below doesn't preserve state).
				bgfx::setScissor(prevScissorID);
			} else {
				prevScissorID = bgfx::setScissor(cmdScissorRect[0], cmdScissorRect[1], cmdScissorRect[2], cmdScissorRect[3]);
				bx::memCopy(prevScissorRect, cmdScissorRect, sizeof(uint16_t) * 4);
			}
		}

		if (cmd->m_Type == DrawCommand::Type_TexturedVertexColor) {
			BX_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid image handle");
			Image* tex = &m_Images[cmd->m_HandleID];
			bgfx::setTexture(0, m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(
				BGFX_STATE_ALPHA_WRITE |
				BGFX_STATE_RGB_WRITE |
				BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));

			bgfx::submit(m_ViewID, m_ProgramHandle[DrawCommand::Type_TexturedVertexColor], cmdDepth, false);
		} else if (cmd->m_Type == DrawCommand::Type_ColorGradient) {
			BX_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid gradient handle");
			Gradient* grad = &m_Gradients[cmd->m_HandleID];

			bgfx::setUniform(m_PaintMatUniform, grad->m_Matrix, 1);
			bgfx::setUniform(m_ExtentRadiusFeatherUniform, grad->m_Params, 1);
			bgfx::setUniform(m_InnerColorUniform, grad->m_InnerColor, 1);
			bgfx::setUniform(m_OuterColorUniform, grad->m_OuterColor, 1);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(
				BGFX_STATE_ALPHA_WRITE |
				BGFX_STATE_RGB_WRITE |
				BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));

			bgfx::submit(m_ViewID, m_ProgramHandle[DrawCommand::Type_ColorGradient], cmdDepth, false);
		} else {
			BX_CHECK(false, "Unknown draw command type");
		}
	}

	// nvgEndFrame
	if (m_FontImageID != 0) {
		ImageHandle fontImage = m_FontImages[m_FontImageID];

		// delete images that smaller than current one
		if (isValid(fontImage)) {
			int iw, ih;
			getImageSize(fontImage, &iw, &ih);

			int j = 0;
			for (int i = 0; i < m_FontImageID; i++) {
				if (isValid(m_FontImages[i])) {
					int nw, nh;
					getImageSize(m_FontImages[i], &nw, &nh);

					if (nw < iw || nh < ih) {
						deleteImage(m_FontImages[i]);
					} else {
						m_FontImages[j++] = m_FontImages[i];
					}
				}
			}

			// make current font image to first
			m_FontImages[j++] = m_FontImages[0];
			m_FontImages[0] = fontImage;
			m_FontImageID = 0;

			// clear all images after j
			for (int i = j; i < MAX_FONT_IMAGES; i++) {
				m_FontImages[i] = VG_INVALID_HANDLE;
			}
		}
	}
}

void Context::beginPath()
{
	const State* state = getState();
	m_Path->reset(state->m_AvgScale, m_TesselationTolerance);
	m_PathVerticesTransformed = false;
}

void Context::moveTo(float x, float y)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->moveTo(x, y);
}

void Context::lineTo(float x, float y)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->lineTo(x, y);
}

void Context::bezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->cubicTo(c1x, c1y, c2x, c2y, x, y);
}

void Context::arcTo(float x1, float y1, float x2, float y2, float radius)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	BX_UNUSED(x1, y1, x2, y2, radius);
	BX_WARN(false, "ArcTo() not implemented yet"); // NOT USED
}

void Context::rect(float x, float y, float w, float h)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->rect(x, y, w, h);
}

void Context::roundedRect(float x, float y, float w, float h, float r)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->roundedRect(x, y, w, h, r);
}

void Context::circle(float cx, float cy, float r)
{
	BX_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->circle(cx, cy, r);
}

void Context::polyline(const Vec2* coords, uint32_t numPoints)
{
	BX_CHECK(!m_PathVerticesTransformed, "Cannot add new vertices to the path after submitting a draw command");
	m_Path->polyline(&coords[0].x, numPoints);
}

void Context::closePath()
{
	BX_CHECK(!m_PathVerticesTransformed, "Cannot add new vertices to the path after submitting a draw command");
	m_Path->close();
}

void Context::transformPath()
{
	if (m_PathVerticesTransformed) {
		return;
	}

	const uint32_t numPathVertices = m_Path->getNumVertices();
	if (numPathVertices > m_NumTransformedPathVertices) {
		m_TransformedPathVertices = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_TransformedPathVertices, sizeof(Vec2) * numPathVertices, 16);
		m_NumTransformedPathVertices = numPathVertices;
	}
	
	const Vec2* pathVertices = (const Vec2*)m_Path->getVertices();
	const State* state = getState();
	batchTransformPositions(pathVertices, numPathVertices, m_TransformedPathVertices, state->m_TransformMtx);
	m_PathVerticesTransformed = true;
}

void Context::fillConvexPath(Color col, bool aa)
{
	transformPath();

	const Vec2* pathVertices = m_TransformedPathVertices;

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t i = 0; i < numSubPaths; ++i) {
		const SubPath* path = &subPaths[i];
		if (path->m_NumVertices < 3) {
			continue;
		}

		if (aa) {
			renderConvexPolygonAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, col);
		} else {
			renderConvexPolygonNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, col);
		}
	}
}

void Context::fillConvexPath(GradientHandle gradient, bool aa)
{
	transformPath();

	const Vec2* pathVertices = m_TransformedPathVertices;

	// TODO: Anti-aliasing of gradient-filled paths
	BX_UNUSED(aa);

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t i = 0; i < numSubPaths; ++i) {
		const SubPath* path = &subPaths[i];
		if (path->m_NumVertices < 3) {
			continue;
		}

		renderConvexPolygonNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, gradient);
	}
}

void Context::fillConvexPath(ImagePatternHandle img, bool aa)
{
	transformPath();

	const Vec2* pathVertices = m_TransformedPathVertices;

	// TODO: Anti-aliasing of textured paths
	BX_UNUSED(aa);
	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t i = 0; i < numSubPaths; ++i) {
		const SubPath* path = &subPaths[i];
		if (path->m_NumVertices < 3) {
			continue;
		}

		renderConvexPolygonNoAA(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, img);
	}
}

void Context::fillConcavePath(Color col, bool aa)
{
	transformPath();

	const Vec2* pathVertices = m_TransformedPathVertices;

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	BX_CHECK(numSubPaths == 1, "Cannot decompose multiple concave paths"); // Only a single concave polygon can be decomposed at a time.
	BX_UNUSED(numSubPaths);

	const SubPath* path = &subPaths[0];
	if (path->m_NumVertices < 3) {
		return;
	}

	decomposeAndFillConcavePolygon(&pathVertices[path->m_FirstVertexID], path->m_NumVertices, col, aa);
}

void Context::strokePath(Color color, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	transformPath();

	const Vec2* pathVertices = m_TransformedPathVertices;

	const State* state = getState();
	const float avgScale = state->m_AvgScale;
	float strokeWidth = clamp(width * avgScale, 0.0f, 200.0f);
	float alphaScale = state->m_GlobalAlpha;
	bool isThin = false;
	if (strokeWidth <= m_FringeWidth) {
		float alpha = clamp(strokeWidth, 0.0f, m_FringeWidth);
		alphaScale *= alpha * alpha;
		strokeWidth = m_FringeWidth;
		isThin = true;
	}

	uint8_t newAlpha = (uint8_t)(alphaScale * ColorRGBA::getAlpha(color));
	if (newAlpha == 0) {
		return;
	}

	const uint32_t c = ColorRGBA::setAlpha(color, newAlpha);

	uint8_t commonID = 0;
	commonID |= ((uint8_t)lineCap) << 1;
	commonID |= ((uint8_t)lineJoin) << 3;

	const uint32_t numPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t iSubPath = 0; iSubPath < numPaths; ++iSubPath) {
		const SubPath* path = &subPaths[iSubPath];
		if (path->m_NumVertices < 2) {
			continue;
		}
		
		const Vec2* vtx = &pathVertices[path->m_FirstVertexID];
		const uint32_t numPathVertices = path->m_NumVertices;
		const bool isClosed = path->m_IsClosed;

		const uint8_t id = commonID | (isClosed ? 0x01 : 0x00);

		if (aa) {
			if (isThin) {
				const uint8_t combo = id >> 1;
				switch (combo) {
				case  0: renderPathStrokeAAThin<LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, c, isClosed);   break;
				case  1: renderPathStrokeAAThin<LineCap::Square, LineJoin::Miter>(vtx, numPathVertices, c, isClosed);   break;
				case  2: renderPathStrokeAAThin<LineCap::Square, LineJoin::Miter>(vtx, numPathVertices, c, isClosed);   break;
				case  4: renderPathStrokeAAThin<LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, c, isClosed);   break;
				case  5: renderPathStrokeAAThin<LineCap::Square, LineJoin::Bevel>(vtx, numPathVertices, c, isClosed);   break;
				case  6: renderPathStrokeAAThin<LineCap::Square, LineJoin::Bevel>(vtx, numPathVertices, c, isClosed);   break;
				case  8: renderPathStrokeAAThin<LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, c, isClosed);   break;
				case  9: renderPathStrokeAAThin<LineCap::Square, LineJoin::Bevel>(vtx, numPathVertices, c, isClosed);   break;
				case 10: renderPathStrokeAAThin<LineCap::Square, LineJoin::Bevel>(vtx, numPathVertices, c, isClosed);   break;
				default:
					BX_WARN(false, "Invalid stroke configuration");
					break;
				}
			} else {
				switch (id) {
				case  0: renderPathStrokeAA<false, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);   break;
				case  1: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);    break;
				case  2: renderPathStrokeAA<false, LineCap::Round, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);  break;
				case  3: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);    break;
				case  4: renderPathStrokeAA<false, LineCap::Square, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c); break;
				case  5: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);    break;
				// 6 to 7 == invalid line cap type
				case  8: renderPathStrokeAA<false, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);   break;
				case  9: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);    break;
				case 10: renderPathStrokeAA<false, LineCap::Round, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);  break;
				case 11: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);    break;
				case 12: renderPathStrokeAA<false, LineCap::Square, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c); break;
				case 13: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);    break;
				// 14 to 15 == invalid line cap type
				case 16: renderPathStrokeAA<false, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);   break;
				case 17: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);    break;
				case 18: renderPathStrokeAA<false, LineCap::Round, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);  break;
				case 19: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);    break;
				case 20: renderPathStrokeAA<false, LineCap::Square, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c); break;
				case 21: renderPathStrokeAA<true, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);    break;
				// 22 to 32 == invalid line join type
				default:
					BX_WARN(false, "Invalid stroke configuration");
					break;
				}
			}
		} else {
			switch (id) {
			case  0: renderPathStrokeNoAA<false, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);   break;
			case  1: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);    break;
			case  2: renderPathStrokeNoAA<false, LineCap::Round, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);  break;
			case  3: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);    break;
			case  4: renderPathStrokeNoAA<false, LineCap::Square, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c); break;
			case  5: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Miter>(vtx, numPathVertices, strokeWidth, c);    break;
			// 6 to 7 == invalid line cap type
			case  8: renderPathStrokeNoAA<false, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);   break;
			case  9: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);    break;
			case 10: renderPathStrokeNoAA<false, LineCap::Round, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);  break;
			case 11: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);    break;
			case 12: renderPathStrokeNoAA<false, LineCap::Square, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c); break;
			case 13: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Round>(vtx, numPathVertices, strokeWidth, c);    break;
			// 14 to 15 == invalid line cap type
			case 16: renderPathStrokeNoAA<false, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);   break;
			case 17: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);    break;
			case 18: renderPathStrokeNoAA<false, LineCap::Round, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);  break;
			case 19: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);    break;
			case 20: renderPathStrokeNoAA<false, LineCap::Square, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c); break;
			case 21: renderPathStrokeNoAA<true, LineCap::Butt, LineJoin::Bevel>(vtx, numPathVertices, strokeWidth, c);    break;
			// 22 to 32 == invalid line join type
			default:
				BX_WARN(false, "Invalid stroke configuration");
				break;
			}
		}
	}
}

// NOTE: In contrast to NanoVG these Gradients are State dependent (the current transformation matrix is baked in the Gradient matrix).
GradientHandle Context::createLinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	if (m_NextGradientID >= VG_CONFIG_MAX_GRADIENTS) {
		return VG_INVALID_HANDLE;
	}

	GradientHandle handle = { (uint16_t)m_NextGradientID++ };

	const State* state = getState();

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

	Gradient* grad = &m_Gradients[handle.idx];
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
	grad->m_Params[3] = bx::fmax(1.0f, d);
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

GradientHandle Context::createBoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	if (m_NextGradientID >= VG_CONFIG_MAX_GRADIENTS) {
		return VG_INVALID_HANDLE;
	}

	GradientHandle handle = { (uint16_t)m_NextGradientID++ };

	const State* state = getState();

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

	Gradient* grad = &m_Gradients[handle.idx];
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
	grad->m_Params[3] = bx::fmax(1.0f, f);
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

GradientHandle Context::createRadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	if (m_NextGradientID >= VG_CONFIG_MAX_GRADIENTS) {
		return VG_INVALID_HANDLE;
	}

	GradientHandle handle = { (uint16_t)m_NextGradientID++ };

	const State* state = getState();

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

	Gradient* grad = &m_Gradients[handle.idx];
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
	grad->m_Params[3] = bx::fmax(1.0f, f);
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

ImagePatternHandle Context::createImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	if (m_NextImagePatternID >= VG_CONFIG_MAX_IMAGE_PATTERNS) {
		return VG_INVALID_HANDLE;
	}

	ImagePatternHandle handle = { (uint16_t)m_NextImagePatternID++ };

	const State* state = getState();

	const float cs = bx::fcos(angle);
	const float sn = bx::fsin(angle);

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

	vg::ImagePattern* pattern = &m_ImagePatterns[handle.idx];
	bx::memCopy(pattern->m_Matrix, inversePatternMatrix, sizeof(float) * 6);
	pattern->m_ImageHandle = image;
	pattern->m_Alpha = alpha;
	
	return handle;
}

bool Context::isImageHandleValid(ImageHandle image)
{
	if (!isValid(image)) {
		return false;
	}

	Image* tex = &m_Images[image.idx];
	return bgfx::isValid(tex->m_bgfxHandle);
}

void Context::pushState()
{
	BX_CHECK(m_CurStateID < MAX_STATE_STACK_SIZE - 1, "State stack overflow");
	bx::memCopy(&m_StateStack[m_CurStateID + 1], &m_StateStack[m_CurStateID], sizeof(State));
	++m_CurStateID;
}

void Context::popState()
{
	BX_CHECK(m_CurStateID > 0, "State stack underflow");
	--m_CurStateID;

	// If the new state has a different scissor rect than the last draw command 
	// force creating a new command.
	if (m_NumDrawCommands != 0) {
		const DrawCommand* lastDrawCommand = &m_DrawCommands[m_NumDrawCommands - 1];
		const uint16_t* lastScissor = &lastDrawCommand->m_ScissorRect[0];
		const float* stateScissor = &m_StateStack[m_CurStateID].m_ScissorRect[0];
		if (lastScissor[0] != (uint16_t)stateScissor[0] ||
			lastScissor[1] != (uint16_t)stateScissor[1] ||
			lastScissor[2] != (uint16_t)stateScissor[2] ||
			lastScissor[3] != (uint16_t)stateScissor[3]) 
		{
			m_ForceNewDrawCommand = true;
		}
	}
}

void Context::resetScissor()
{
	State* state = getState();
	state->m_ScissorRect[0] = state->m_ScissorRect[1] = 0.0f;
	state->m_ScissorRect[2] = (float)m_WinWidth;
	state->m_ScissorRect[3] = (float)m_WinHeight;
	m_ForceNewDrawCommand = true;
}

void Context::setScissor(float x, float y, float w, float h)
{
	State* state = getState();
	Vec2 pos = transformPos2D(x, y, state->m_TransformMtx);
	Vec2 size = transformVec2D(w, h, state->m_TransformMtx);
	state->m_ScissorRect[0] = pos.x;
	state->m_ScissorRect[1] = pos.y;
	state->m_ScissorRect[2] = size.x;
	state->m_ScissorRect[3] = size.y;
	m_ForceNewDrawCommand = true;
}

bool Context::intersectScissor(float x, float y, float w, float h)
{
	State* state = getState();
	Vec2 pos = transformPos2D(x, y, state->m_TransformMtx);
	Vec2 size = transformVec2D(w, h, state->m_TransformMtx);

	const float* rect = state->m_ScissorRect;

	float minx = bx::fmax(pos.x, rect[0]);
	float miny = bx::fmax(pos.y, rect[1]);
	float maxx = bx::fmin(pos.x + size.x, rect[0] + rect[2]);
	float maxy = bx::fmin(pos.y + size.y, rect[1] + rect[3]);

	state->m_ScissorRect[0] = minx;
	state->m_ScissorRect[1] = miny;
	state->m_ScissorRect[2] = bx::fmax(0.0f, maxx - minx);
	state->m_ScissorRect[3] = bx::fmax(0.0f, maxy - miny);

	m_ForceNewDrawCommand = true;

	// Return false in case scissor rect is too small for bgfx to handle correctly
	// NOTE: This was originally required in NanoVG because the scissor rectangle is passed
	// to the fragment shader. In this case, we might use it to discard anything that will
	// be rendered after this call.
	return state->m_ScissorRect[2] >= 1.0f && state->m_ScissorRect[3] >= 1.0f;
}

void Context::onTransformationMatrixUpdated()
{
	State* state = getState();
	state->m_FontScale = getFontScale(state->m_TransformMtx);
	state->m_AvgScale = getAverageScale(state->m_TransformMtx);
}

void Context::transformIdentity()
{
	State* state = getState();
	state->m_TransformMtx[0] = 1.0f;
	state->m_TransformMtx[1] = 0.0f;
	state->m_TransformMtx[2] = 0.0f;
	state->m_TransformMtx[3] = 1.0f;
	state->m_TransformMtx[4] = 0.0f;
	state->m_TransformMtx[5] = 0.0f;

	onTransformationMatrixUpdated();
}

void Context::transformScale(float x, float y)
{
	State* state = getState();
	state->m_TransformMtx[0] = x * state->m_TransformMtx[0];
	state->m_TransformMtx[1] = x * state->m_TransformMtx[1];
	state->m_TransformMtx[2] = y * state->m_TransformMtx[2];
	state->m_TransformMtx[3] = y * state->m_TransformMtx[3];

	onTransformationMatrixUpdated();
}

void Context::transformTranslate(float x, float y)
{
	State* state = getState();
	state->m_TransformMtx[4] += state->m_TransformMtx[0] * x + state->m_TransformMtx[2] * y;
	state->m_TransformMtx[5] += state->m_TransformMtx[1] * x + state->m_TransformMtx[3] * y;

	onTransformationMatrixUpdated();
}

void Context::transformRotate(float ang_rad)
{
	const float c = bx::fcos(ang_rad);
	const float s = bx::fsin(ang_rad);

	State* state = getState();
	float mtx[6];
	mtx[0] = c * state->m_TransformMtx[0] + s * state->m_TransformMtx[2];
	mtx[1] = c * state->m_TransformMtx[1] + s * state->m_TransformMtx[3];
	mtx[2] = -s * state->m_TransformMtx[0] + c * state->m_TransformMtx[2];
	mtx[3] = -s * state->m_TransformMtx[1] + c * state->m_TransformMtx[3];
	mtx[4] = state->m_TransformMtx[4];
	mtx[5] = state->m_TransformMtx[5];
	bx::memCopy(state->m_TransformMtx, mtx, sizeof(float) * 6);

	onTransformationMatrixUpdated();
}

void Context::transformMult(const float* mtx, bool pre)
{
	State* state = getState();

	float res[6];
	if (pre) {
		multiplyMatrix3(state->m_TransformMtx, mtx, res);
	} else {
		multiplyMatrix3(mtx, state->m_TransformMtx, res);
	}

	bx::memCopy(state->m_TransformMtx, res, sizeof(float) * 6);

	onTransformationMatrixUpdated();
}

void Context::setGlobalAlpha(float alpha)
{
	getState()->m_GlobalAlpha = alpha;
}

void Context::text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float scaledFontSize = font.m_Size * scale;
	if (scaledFontSize < 1.0f) { // TODO: Make threshold configurable.
		return;
	}

	if (!end) {
		end = text + bx::strLen(text);
	}

	if (end == text) {
		return;
	}

	fonsSetSize(m_FontStashContext, scaledFontSize);
//	fonsSetAlign(m_FontStashContext, alignment);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);

	fonsResetString(m_FontStashContext, &m_TextString, text, end);

	uint32_t numBakedChars = fonsBakeString(m_FontStashContext, &m_TextString);
	if (numBakedChars == ~0u) {
		// Atlas full? Retry
		if (!allocTextAtlas()) {
			return;
		}

		numBakedChars = fonsBakeString(m_FontStashContext, &m_TextString);
	}

	if (numBakedChars == ~0u || numBakedChars == 0) {
		return;
	}

	if (m_TextQuadCapacity < numBakedChars) {
		m_TextQuadCapacity = numBakedChars;
		m_TextQuads = (FONSquad*)BX_ALIGNED_REALLOC(m_Allocator, m_TextQuads, sizeof(FONSquad) * m_TextQuadCapacity, 16);
		m_TextVertices = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_TextVertices, sizeof(Vec2) * m_TextQuadCapacity * 4, 16);
	}

	bx::memCopy(m_TextQuads, m_TextString.m_Quads, sizeof(FONSquad) * numBakedChars);

	float dx = 0.0f, dy = 0.0f;
	fonsAlignString(m_FontStashContext, &m_TextString, alignment, &dx, &dy);

	pushState();
	transformTranslate(x + dx / scale, y + dy / scale);
	renderTextQuads(numBakedChars, color);
	popState();

//	// Assume ASCII where each byte is a codepoint. Otherwise, the number 
//	// of quads will be less than the number of chars/bytes.
//	const uint32_t maxChars = (uint32_t)(end - text);
//	if (m_TextQuadCapacity < maxChars) {
//		m_TextQuadCapacity = maxChars;
//		m_TextQuads = (FONSquad*)BX_ALIGNED_REALLOC(m_Allocator, m_TextQuads, sizeof(FONSquad) * m_TextQuadCapacity, 16);
//		m_TextVertices = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_TextVertices, sizeof(Vec2) * m_TextQuadCapacity * 4, 16);
//	}
//	
//	FONStextIter iter;
//	fonsTextIterInit(m_FontStashContext, &iter, x * scale, y * scale, text, end);
//
//	FONSquad* nextQuad = m_TextQuads;
//	FONStextIter prevIter = iter;
//	while (fonsTextIterNext(m_FontStashContext, &iter, nextQuad)) {
//		if (iter.prevGlyphIndex == -1) {
//			// Draw all quads up to this point (if any) with the current atlas texture.
//			const uint32_t numQuads = (uint32_t)(nextQuad - m_TextQuads);
//			if (numQuads != 0) {
//				BX_CHECK(numQuads <= m_TextQuadCapacity, "Text command generated more quads than the temp buffer can hold");
//
//				renderTextQuads(numQuads, color);
//
//				// Reset next quad ptr.
//				nextQuad = m_TextQuads;
//			}
//
//			// Allocate a new atlas
//			if (!allocTextAtlas()) {
//				break;
//			}
//
//			// And try fitting the glyph once again.
//			iter = prevIter;
//			fonsTextIterNext(m_FontStashContext, &iter, nextQuad);
//			if (iter.prevGlyphIndex == -1) {
//				break;
//			}
//		}
//
//		++nextQuad;
//		prevIter = iter;
//	}
//
//	const uint32_t numQuads = (uint32_t)(nextQuad - m_TextQuads);
//	if (numQuads == 0) {
//		return;
//	}
//
//	BX_CHECK(numQuads <= m_TextQuadCapacity, "Text command generated more quads than the temp buffer can hold");
//
//	renderTextQuads(numQuads, color);
}

void Context::textBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* str, const char* end)
{
	int halign = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	int valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);

	float lineh = getTextLineHeight(font, alignment);

	alignment = FONS_ALIGN_LEFT | valign;

	TextRow rows[2];
	int nrows;
	while ((nrows = textBreakLines(font, alignment, str, end, breakWidth, rows, 2, 0))) {
		for (int i = 0; i < nrows; ++i) {
			TextRow* row = &rows[i];

			if (halign & FONS_ALIGN_LEFT) {
				text(font, alignment, color, x, y, row->start, row->end);
			} else if (halign & FONS_ALIGN_CENTER) {
				text(font, alignment, color, x + breakWidth * 0.5f - row->width * 0.5f, y, row->start, row->end);
			} else if (halign & FONS_ALIGN_RIGHT) {
				text(font, alignment, color, x + breakWidth - row->width, y, row->start, row->end);
			}

			y += lineh; // Assume line height multiplier to be 1.0 (NanoVG allows the user to change it, but I don't use it).
		}

		str = rows[nrows - 1].next;
	}
}

float Context::calcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds)
{
	// nvgTextBounds()
	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	fonsSetSize(m_FontStashContext, font.m_Size * scale);
	fonsSetAlign(m_FontStashContext, alignment);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);

	float width = fonsTextBounds(m_FontStashContext, x * scale, y * scale, text, end, bounds);
	if (bounds != nullptr) {
		// Use line bounds for height.
		fonsLineBounds(m_FontStashContext, y * scale, &bounds[1], &bounds[3]);
		bounds[0] *= invscale;
		bounds[1] *= invscale;
		bounds[2] *= invscale;
		bounds[3] *= invscale;
	}

	return width * invscale;
}

void Context::calcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	const int halign = alignment & (FONS_ALIGN_LEFT | FONS_ALIGN_CENTER | FONS_ALIGN_RIGHT);
	const int valign = alignment & (FONS_ALIGN_TOP | FONS_ALIGN_MIDDLE | FONS_ALIGN_BOTTOM | FONS_ALIGN_BASELINE);

	alignment = FONS_ALIGN_LEFT | valign;
	fonsSetAlign(m_FontStashContext, alignment);
	fonsSetSize(m_FontStashContext, font.m_Size * scale);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);

	float lineh;
	fonsVertMetrics(m_FontStashContext, nullptr, nullptr, &lineh);
	lineh *= invscale;

	float rminy = 0, rmaxy = 0;
	fonsLineBounds(m_FontStashContext, 0, &rminy, &rmaxy);
	rminy *= invscale;
	rmaxy *= invscale;

	float minx, miny, maxx, maxy;
	minx = maxx = x;
	miny = maxy = y;

	TextRow rows[2];
	int nrows = 0;
	while ((nrows = textBreakLines(font, alignment, text, end, breakWidth, rows, 2, flags))) {
		for (int i = 0; i < nrows; i++) {
			TextRow* row = &rows[i];
			float rminx, rmaxx;
	
			// Horizontal bounds
			float dx = 0.0f; // Assume left align
			if (halign & FONS_ALIGN_CENTER) {
				dx = breakWidth * 0.5f - row->width * 0.5f;
			} else if (halign & FONS_ALIGN_RIGHT) {
				dx = breakWidth - row->width;
			}

			rminx = x + row->minx + dx;
			rmaxx = x + row->maxx + dx;

			minx = bx::fmin(minx, rminx);
			maxx = bx::fmax(maxx, rmaxx);

			// Vertical bounds.
			miny = bx::fmin(miny, y + rminy);
			maxy = bx::fmax(maxy, y + rmaxy);

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

float Context::getTextLineHeight(const Font& font, uint32_t alignment)
{
	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	fonsSetSize(m_FontStashContext, font.m_Size * scale);
	fonsSetAlign(m_FontStashContext, alignment);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);

	float lineh;
	fonsVertMetrics(m_FontStashContext, nullptr, nullptr, &lineh);
	lineh *= invscale;

	return lineh;
}

int Context::textBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	// nvgTextBreakLines()
#define CP_SPACE  0
#define CP_NEW_LINE  1
#define CP_CHAR 2

	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	if (maxRows == 0) {
		return 0;
	}

	if (end == nullptr) {
		end = text + bx::strLen(text);
	}

	if (text == end) {
		return 0;
	}

	fonsSetSize(m_FontStashContext, font.m_Size * scale);
	fonsSetAlign(m_FontStashContext, alignment);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);

	breakRowWidth *= scale;

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

	FONStextIter iter, prevIter;
	fonsTextIterInit(m_FontStashContext, &iter, 0, 0, text, end);
	prevIter = iter;

	FONSquad q;
	while (fonsTextIterNext(m_FontStashContext, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && allocTextAtlas()) { 
			// can not retrieve glyph?
			iter = prevIter;
			fonsTextIterNext(m_FontStashContext, &iter, &q); // try again
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

int Context::textGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions)
{
	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float invscale = 1.0f / scale;

	if (!end) {
		end = text + bx::strLen(text);
	}

	if (text == end) {
		return 0;
	}

	fonsSetSize(m_FontStashContext, font.m_Size * scale);
	fonsSetAlign(m_FontStashContext, alignment);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);

	FONStextIter iter, prevIter;
	fonsTextIterInit(m_FontStashContext, &iter, x * scale, y * scale, text, end);
	prevIter = iter;

	FONSquad q;
	int npos = 0;
	while (fonsTextIterNext(m_FontStashContext, &iter, &q)) {
		if (iter.prevGlyphIndex < 0 && allocTextAtlas()) {
			iter = prevIter;
			fonsTextIterNext(m_FontStashContext, &iter, &q);
		}

		prevIter = iter;
		positions[npos].str = iter.str;
		positions[npos].x = iter.x * invscale;
		positions[npos].minx = bx::fmin(iter.x, q.x0) * invscale;
		positions[npos].maxx = bx::fmax(iter.nextx, q.x1) * invscale;
		
		npos++;
		if (npos >= maxPositions) {
			break;
		}
	}

	return npos;
}

template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void Context::renderPathStrokeAA(const Vec2* vtx, uint32_t numPathVertices, float strokeWidth, Color color)
{
	const Vec2 uv = getWhitePixelUV();
	const float avgScale = getState()->m_AvgScale;
	const uint32_t numSegments = numPathVertices - (_Closed ? 0 : 1);
	const uint32_t c0 = ColorRGBA::setAlpha(color, 0);
	const uint32_t c0_c_c_c0[4] = { c0, color, color, c0 };
	const float hsw = (strokeWidth - m_FringeWidth) * 0.5f;
	const float hsw_aa = hsw + m_FringeWidth;
	const float da = bx::facos((avgScale * hsw) / ((avgScale * hsw) + m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2u, (uint32_t)bx::fceil(bx::kPi / da));

	tempGeomReset();

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

		d01 = p1 - p0;
		vec2Normalize(&d01);

		const Vec2 l01 = d01.perpCCW();

		if (_LineCap == LineCap::Butt) {
			const Vec2 l01_hsw = l01 * hsw;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;
			const Vec2 d01_aa = d01 * m_FringeWidth;

			Vec2 p[4] = {
				p0 + l01_hsw_aa - d01_aa,
				p0 + l01_hsw,
				p0 - l01_hsw,
				p0 - l01_hsw_aa - d01_aa
			};

			tempGeomExpandVB(4);
			tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 4);

			uint16_t id[6] = {
				0, 2, 1,
				0, 3, 2
			};
			tempGeomExpandIB(6);
			tempGeomAddIndices(&id[0], 6);

			prevSegmentLeftAAID = 0;
			prevSegmentLeftID = 1;
			prevSegmentRightID = 2;
			prevSegmentRightAAID = 3;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 l01_hsw = l01 * hsw;
			const Vec2 d01_hsw = d01 * hsw;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;
			const Vec2 d01_hsw_aa = d01 * hsw_aa;

			Vec2 p[4] = {
				p0 + l01_hsw_aa - d01_hsw_aa,
				p0 + l01_hsw - d01_hsw,
				p0 - l01_hsw - d01_hsw,
				p0 - l01_hsw_aa - d01_hsw_aa
			};

			tempGeomExpandVB(4);
			tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 4);

			uint16_t id[6] = {
				0, 2, 1,
				0, 3, 2
			};
			tempGeomExpandIB(6);
			tempGeomAddIndices(&id[0], 6);

			prevSegmentLeftAAID = 0;
			prevSegmentLeftID = 1;
			prevSegmentRightID = 2;
			prevSegmentRightAAID = 3;
		} else if (_LineCap == LineCap::Round) {
			const float startAngle = atan2f(l01.y, l01.x);
			tempGeomExpandVB(numPointsHalfCircle << 1);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle + i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p[2] = {
					Vec2(p0.x + ca * hsw, p0.y + sa * hsw),
					Vec2(p0.x + ca * hsw_aa, p0.y + sa * hsw_aa)
				};

				tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
			}

			// Generate indices for the triangle fan
			tempGeomExpandIB(numPointsHalfCircle * 9 - 12);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				uint16_t id[3] = {
					0,
					(uint16_t)((i << 1) + 2),
					(uint16_t)((i << 1) + 4)
				};
				tempGeomAddIndices(&id[0], 3);
			}

			// Generate indices for the AA quads
			for (uint32_t i = 0; i < numPointsHalfCircle - 1; ++i) {
				const uint16_t idBase = (uint16_t)(i << 1);
				uint16_t id[6] = {
					idBase, (uint16_t)(idBase + 1), (uint16_t)(idBase + 3),
					idBase, (uint16_t)(idBase + 3), (uint16_t)(idBase + 2)
				};
				tempGeomAddIndices(&id[0], 6);
			}

			prevSegmentLeftAAID = 1;
			prevSegmentLeftID = 0;
			prevSegmentRightID = (uint16_t)((numPointsHalfCircle - 1) * 2);
			prevSegmentRightAAID = (uint16_t)((numPointsHalfCircle - 1) * 2 + 1);
		} else {
			BX_CHECK(false, "Unknown line cap type");
		}
	} else {
		d01 = vtx[0] - vtx[numPathVertices - 1];
		vec2Normalize(&d01);
	}

	const uint32_t firstSegmentID = _Closed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		Vec2 d12 = p2 - p1;
		vec2Normalize(&d12);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_hsw_aa = v * hsw_aa;

		// Check which one of the points is the inner corner.
		float leftPointAAProjDist = d12.x * v_hsw_aa.x + d12.y * v_hsw_aa.y;
		if (leftPointAAProjDist >= 0.0f) {
			// The left point is the inner corner.
			const Vec2 v_hsw = v * hsw;
			const Vec2 innerCornerAA = p1 + v_hsw_aa;
			const Vec2 innerCorner = p1 + v_hsw;

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)m_TempGeomNumVertices;

				Vec2 p[4] = {
					innerCornerAA,
					innerCorner,
					p1 - v_hsw,
					p1 - v_hsw_aa
				};

				tempGeomExpandVB(4);
				tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 4);

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstVertexID + 1), firstVertexID,
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstVertexID + 2),
						prevSegmentLeftID, (uint16_t)(firstVertexID + 2), (uint16_t)(firstVertexID + 1),
						prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(firstVertexID + 3),
						prevSegmentRightID, (uint16_t)(firstVertexID + 3), (uint16_t)(firstVertexID + 2)
					};

					tempGeomExpandIB(18);
					tempGeomAddIndices(&id[0], 18);
				} else {
					BX_CHECK(_Closed, "Invalid previous segment");
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
				const Vec2 r01 = d01.perpCW();
				const Vec2 r12 = d12.perpCW();

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = atan2f(r01.y, r01.x);
					a12 = atan2f(r12.y, r12.x);
					if (a12 < a01) {
						a12 += bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a12 - a01) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				const uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;
				tempGeomExpandVB(numArcPoints * 2 + 4);

				Vec2 p[2] = {
					innerCornerAA,
					innerCorner
				};
				tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 2);

				// First arc vertex
				{
					Vec2 p[2] = {
						p1 + r01 * hsw, 
						p1 + r01 * hsw_aa
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(r01.dot(r12));
						p[0] -= d01 * (cosAngle * m_FringeWidth);
					}

					tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Middle arc vertices
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;

					const Vec2 arcPointDir(bx::fcos(a), bx::fsin(a));

					Vec2 p[2] = {
						p1 + arcPointDir * hsw,
						p1 + arcPointDir * hsw_aa
					};

					tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Last arc vertex
				{
					Vec2 p[2] = {
						p1 + r12 * hsw,
						p1 + r12 * hsw_aa
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(r01.dot(r12));
						p[0] += d12 * (cosAngle * m_FringeWidth);
					}

					tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID,
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 1),
						prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(firstFanVertexID + 3),
						prevSegmentRightID, (uint16_t)(firstFanVertexID + 3), (uint16_t)(firstFanVertexID + 2)
					};

					tempGeomExpandIB(18);
					tempGeomAddIndices(&id[0], 18);
				} else {
					BX_CHECK(_Closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstFanVertexID; // 0
					firstSegmentLeftID = firstFanVertexID + 1; // 1
					firstSegmentRightID = firstFanVertexID + 2; // 2
					firstSegmentRightAAID = firstFanVertexID + 3; // 3
				}

				// Generate the slice.
				uint16_t arcID = firstFanVertexID + 2;
				tempGeomExpandIB(numArcPoints * 9);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					uint16_t id[9] = {
						(uint16_t)(firstFanVertexID + 1), arcID, (uint16_t)(arcID + 2),
						arcID, (uint16_t)(arcID + 1), (uint16_t)(arcID + 3),
						arcID, (uint16_t)(arcID + 3), (uint16_t)(arcID + 2)
					};
					tempGeomAddIndices(&id[0], 9);

					arcID += 2;
				}

				prevSegmentLeftAAID = firstFanVertexID;
				prevSegmentLeftID = firstFanVertexID + 1;
				prevSegmentRightID = arcID;
				prevSegmentRightAAID = arcID + 1;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 v_hsw = v * hsw;
			const Vec2 innerCornerAA = p1 - v_hsw_aa;
			const Vec2 innerCorner = p1 - v_hsw;

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;

				Vec2 p[4] = {
					innerCornerAA,
					innerCorner,
					p1 + v_hsw,
					p1 + v_hsw_aa
				};

				tempGeomExpandVB(4);
				tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 4);

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3),
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentRightID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentRightID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					tempGeomExpandIB(18);
					tempGeomAddIndices(&id[0], 18);
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
				const Vec2 l01 = d01.perpCCW();
				const Vec2 l12 = d12.perpCCW();

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = atan2f(l01.y, l01.x);
					a12 = atan2f(l12.y, l12.x);
					if (a12 > a01) {
						a12 -= bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a01 - a12) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				const uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;
				tempGeomExpandVB(numArcPoints * 2 + 4);

				Vec2 p[2] = {
					innerCornerAA,
					innerCorner
				};
				tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 2);

				// First arc vertex
				{
					Vec2 p[2] = {
						p1 + l01 * hsw,
						p1 + l01 * hsw_aa
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(l01.dot(l12));
						p[0] -= d01 * (cosAngle * m_FringeWidth);
					}

					tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Middle arc vertices
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;

					const Vec2 arcPointDir(bx::fcos(a), bx::fsin(a));

					Vec2 p[2] = {
						p1 + arcPointDir * hsw,
						p1 + arcPointDir * hsw_aa
					};

					tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				// Last arc vertex
				{
					Vec2 p[2] = {
						p1 + l12 * hsw,
						p1 + l12 * hsw_aa
					};

					if (_LineJoin == LineJoin::Bevel) {
						const float cosAngle = bx::fabs(l01.dot(l12));
						p[0] += d12 * (cosAngle * m_FringeWidth);
					}

					tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
				}

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[18] = {
						prevSegmentLeftAAID, prevSegmentLeftID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3),
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentRightID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentRightID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					tempGeomExpandIB(18);
					tempGeomAddIndices(&id[0], 18);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 3; // 3
					firstSegmentLeftID = firstFanVertexID + 2; // 2
					firstSegmentRightID = firstFanVertexID + 1; // 1
					firstSegmentRightAAID = firstFanVertexID + 0; // 0
				}

				// Generate the slice.
				uint16_t arcID = firstFanVertexID + 2;
				tempGeomExpandIB(numArcPoints * 9);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					uint16_t id[9] = {
						(uint16_t)(firstFanVertexID + 1), (uint16_t)(arcID + 2), arcID,
						arcID, (uint16_t)(arcID + 3), (uint16_t)(arcID + 1),
						arcID, (uint16_t)(arcID + 2), (uint16_t)(arcID + 3)
					};
					tempGeomAddIndices(&id[0], 9);

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

		const Vec2 l01 = d01.perpCCW();

		if (_LineCap == LineCap::Butt) {
			const uint16_t curSegmentLeftAAID = (uint16_t)m_TempGeomNumVertices;
			const Vec2 l01_hsw = l01 * hsw;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;
			const Vec2 d01_aa = d01 * m_FringeWidth;

			Vec2 p[4] = {
				p1 + l01_hsw_aa + d01_aa,
				p1 + l01_hsw,
				p1 - l01_hsw,
				p1 - l01_hsw_aa + d01_aa
			};

			tempGeomExpandVB(4);
			tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 4);

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

			tempGeomExpandIB(24);
			tempGeomAddIndices(&id[0], 24);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftAAID = (uint16_t)m_TempGeomNumVertices;
			const Vec2 l01_hsw = l01 * hsw;
			const Vec2 d01_hsw = d01 * hsw;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;
			const Vec2 d01_hsw_aa = d01 * hsw_aa;

			Vec2 p[4] = {
				p1 + l01_hsw_aa + d01_hsw_aa,
				p1 + l01_hsw + d01_hsw,
				p1 - l01_hsw + d01_hsw,
				p1 - l01_hsw_aa + d01_hsw_aa
			};

			tempGeomExpandVB(4);
			tempGeomAddPosColor(&p[0], &c0_c_c_c0[0], 4);

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

			tempGeomExpandIB(24);
			tempGeomAddIndices(&id[0], 24);
		} else if (_LineCap == LineCap::Round) {
			const uint16_t curSegmentLeftID = (uint16_t)m_TempGeomNumVertices;
			const float startAngle = atan2f(l01.y, l01.x);

			tempGeomExpandVB(numPointsHalfCircle * 2);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle - i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p[2] = {
					Vec2(p1.x + ca * hsw, p1.y + sa * hsw),
					Vec2(p1.x + ca * hsw_aa, p1.y + sa * hsw_aa)
				};

				tempGeomAddPosColor(&p[0], &c0_c_c_c0[2], 2);
			}

			uint16_t id[18] = {
				prevSegmentLeftAAID, prevSegmentLeftID, curSegmentLeftID,
				prevSegmentLeftAAID, curSegmentLeftID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2), curSegmentLeftID,
				prevSegmentRightID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2 + 1),
				prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2 + 1), (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1) * 2)
			};

			tempGeomExpandIB(18);
			tempGeomAddIndices(&id[0], 18);

			// Generate indices for the triangle fan
			tempGeomExpandIB((numPointsHalfCircle - 2) * 3);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)(i << 1);
				uint16_t id[3] = {
					curSegmentLeftID,
					(uint16_t)(idBase + 4),
					(uint16_t)(idBase + 2)
				};
				tempGeomAddIndices(&id[0], 3);
			}

			// Generate indices for the AA quads
			tempGeomExpandIB((numPointsHalfCircle - 1) * 6);
			for (uint32_t i = 0; i < numPointsHalfCircle - 1; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)(i << 1);
				uint16_t id[6] = {
					idBase, (uint16_t)(idBase + 3), (uint16_t)(idBase + 1),
					idBase, (uint16_t)(idBase + 2), (uint16_t)(idBase + 3)
				};
				tempGeomAddIndices(&id[0], 6);
			}
		}
	} else {
		BX_CHECK(firstSegmentLeftAAID != 0xFFFF && firstSegmentLeftID != 0xFFFF && firstSegmentRightID != 0xFFFF && firstSegmentRightAAID != 0xFFFF, "Invalid first segment");

		uint16_t id[18] = {
			prevSegmentLeftAAID, prevSegmentLeftID, firstSegmentLeftID,
			prevSegmentLeftAAID, firstSegmentLeftID, firstSegmentLeftAAID,
			prevSegmentLeftID, prevSegmentRightID, firstSegmentRightID,
			prevSegmentLeftID, firstSegmentRightID, firstSegmentLeftID,
			prevSegmentRightID, prevSegmentRightAAID, firstSegmentRightAAID,
			prevSegmentRightID, firstSegmentRightAAID, firstSegmentRightID
		};

		tempGeomExpandIB(18);
		tempGeomAddIndices(&id[0], 18);
	}

	// Create a new draw command and copy the data to it.
	// TODO: Turn this into a function.
	const uint32_t numDrawVertices = m_TempGeomNumVertices;
	const uint32_t numDrawIndices = m_TempGeomNumIndices;
	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, m_TempGeomPos, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	memset64(dstUV, numDrawVertices, &uv.x);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	bx::memCopy(dstColor, m_TempGeomColor, sizeof(uint32_t) * numDrawVertices);

	const uint32_t startVertexID = cmd->m_NumVertices;
	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(m_TempGeomIndex, m_TempGeomNumIndices, dstIndex, (uint16_t)startVertexID);

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

template<LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void Context::renderPathStrokeAAThin(const Vec2* vtx, uint32_t numPathVertices, Color color, bool closed)
{
	const Vec2 uv = getWhitePixelUV();
	const uint32_t numSegments = numPathVertices - (closed ? 0 : 1);
	const uint32_t c0 = ColorRGBA::setAlpha(color, 0);
	const uint32_t c0_c_c0_c0[4] = { c0, color, c0, c0 };
	const float hsw_aa = m_FringeWidth;

	tempGeomReset();

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

		d01 = p1 - p0;
		vec2Normalize(&d01);

		const Vec2 l01 = d01.perpCCW();

		if (_LineCap == LineCap::Butt) {
			const Vec2 l01_hsw_aa = l01 * hsw_aa;

			Vec2 p[3] = {
				p0 + l01_hsw_aa,
				p0,
				p0 - l01_hsw_aa
			};

			tempGeomExpandVB(3);
			tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 3);

			prevSegmentLeftAAID = 0;
			prevSegmentMiddleID = 1;
			prevSegmentRightAAID = 2;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 d01_hsw_aa = d01 * hsw_aa;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;

			Vec2 p[4] = {
				p0 + l01_hsw_aa - d01_hsw_aa,
				p0,
				p0 - l01_hsw_aa - d01_hsw_aa
			};

			tempGeomExpandVB(3);
			tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 3);

			prevSegmentLeftAAID = 0;
			prevSegmentMiddleID = 1;
			prevSegmentRightAAID = 2;
		} else if (_LineCap == LineCap::Round) {
			BX_CHECK(false, "Round caps not implemented for thin strokes.");
		} else {
			BX_CHECK(false, "Unknown line cap type");
		}
	} else {
		d01 = vtx[0] - vtx[numPathVertices - 1];
		vec2Normalize(&d01);
	}

	const uint32_t firstSegmentID = closed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		Vec2 d12 = p2 - p1;
		vec2Normalize(&d12);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_hsw_aa = v * hsw_aa;

		// Check which one of the points is the inner corner.
		float leftPointAAProjDist = d12.x * v_hsw_aa.x + d12.y * v_hsw_aa.y;
		if (leftPointAAProjDist >= 0.0f) {
			// The left point is the inner corner.
			const Vec2 innerCorner = p1 + v_hsw_aa;

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)m_TempGeomNumVertices;

				Vec2 p[3] = {
					innerCorner,
					p1,
					p1 - v_hsw_aa
				};

				tempGeomExpandVB(3);
				tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 3);

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstVertexID + 1), firstVertexID,
						prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(firstVertexID + 2),
						prevSegmentMiddleID, (uint16_t)(firstVertexID + 2), (uint16_t)(firstVertexID + 1)
					};

					tempGeomExpandIB(12);
					tempGeomAddIndices(&id[0], 12);
				} else {
					BX_CHECK(closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstVertexID; // 0
					firstSegmentMiddleID = firstVertexID + 1; // 1
					firstSegmentRightAAID = firstVertexID + 2; // 3
				}

				prevSegmentLeftAAID = firstVertexID;
				prevSegmentMiddleID = firstVertexID + 1;
				prevSegmentRightAAID = firstVertexID + 2;
			} else {
				BX_CHECK(_LineJoin != LineJoin::Round, "Round joins not implemented for thin strokes.");
				const Vec2 r01 = d01.perpCW();
				const Vec2 r12 = d12.perpCW();

				Vec2 p[4] = {
					innerCorner,
					p1,
					p1 + r01 * hsw_aa,
					p1 + r12 * hsw_aa
				};

				const uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;
				tempGeomExpandVB(4);
				tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 4);

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID,
						prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 1)
					};

					tempGeomExpandIB(12);
					tempGeomAddIndices(&id[0], 12);
				} else {
					BX_CHECK(closed, "Invalid previous segment");
					firstSegmentLeftAAID = firstFanVertexID;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 2;
				}

				uint16_t id[3] = {
					(uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2), (uint16_t)(firstFanVertexID + 3)
				};
				tempGeomExpandIB(3);
				tempGeomAddIndices(&id[0], 3);

				prevSegmentLeftAAID = firstFanVertexID;
				prevSegmentMiddleID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID + 3;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 innerCorner = p1 - v_hsw_aa;

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;

				Vec2 p[3] = {
					innerCorner,
					p1,
					p1 + v_hsw_aa
				};

				tempGeomExpandVB(3);
				tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 3);

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, prevSegmentRightAAID, firstFanVertexID,
						prevSegmentMiddleID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					tempGeomExpandIB(12);
					tempGeomAddIndices(&id[0], 12);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 2;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				prevSegmentLeftAAID = firstFanVertexID + 2;
				prevSegmentMiddleID = firstFanVertexID + 1;
				prevSegmentRightAAID = firstFanVertexID;
			} else {
				const Vec2 l01 = d01.perpCCW();
				const Vec2 l12 = d12.perpCCW();

				Vec2 p[4] = {
					innerCorner,
					p1,
					p1 + l01 * hsw_aa,
					p1 + l12 * hsw_aa
				};

				const uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;
				tempGeomExpandVB(4);
				tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 4);

				if (prevSegmentLeftAAID != 0xFFFF) {
					BX_CHECK(prevSegmentMiddleID != 0xFFFF && prevSegmentRightAAID != 0xFFFF, "Invalid previous segment");

					uint16_t id[12] = {
						prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftAAID, (uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 2),
						prevSegmentMiddleID, prevSegmentRightAAID, firstFanVertexID ,
						prevSegmentMiddleID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					tempGeomExpandIB(12);
					tempGeomAddIndices(&id[0], 12);
				} else {
					firstSegmentLeftAAID = firstFanVertexID + 2;
					firstSegmentMiddleID = firstFanVertexID + 1;
					firstSegmentRightAAID = firstFanVertexID + 0;
				}

				uint16_t id[3] = {
					(uint16_t)(firstFanVertexID + 1), (uint16_t)(firstFanVertexID + 3), (uint16_t)(firstFanVertexID + 2)
				};
				tempGeomAddIndices(&id[0], 3);

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

		const Vec2 l01 = d01.perpCCW();

		if (_LineCap == LineCap::Butt) {
			const uint16_t curSegmentLeftAAID = (uint16_t)m_TempGeomNumVertices;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;

			Vec2 p[3] = {
				p1 + l01_hsw_aa,
				p1,
				p1 - l01_hsw_aa
			};

			tempGeomExpandVB(3);
			tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 3);

			uint16_t id[12] = {
				prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1)
			};

			tempGeomExpandIB(12);
			tempGeomAddIndices(&id[0], 12);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftAAID = (uint16_t)m_TempGeomNumVertices;
			const Vec2 d01_hsw = d01 * hsw_aa;
			const Vec2 l01_hsw_aa = l01 * hsw_aa;

			Vec2 p[3] = {
				p1 + l01_hsw_aa + d01_hsw,
				p1,
				p1 - l01_hsw_aa + d01_hsw
			};

			tempGeomExpandVB(3);
			tempGeomAddPosColor(&p[0], &c0_c_c0_c0[0], 3);

			uint16_t id[12] = {
				prevSegmentLeftAAID, prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 1),
				prevSegmentLeftAAID, (uint16_t)(curSegmentLeftAAID + 1), curSegmentLeftAAID,
				prevSegmentMiddleID, prevSegmentRightAAID, (uint16_t)(curSegmentLeftAAID + 2),
				prevSegmentMiddleID, (uint16_t)(curSegmentLeftAAID + 2), (uint16_t)(curSegmentLeftAAID + 1)
			};

			tempGeomExpandIB(12);
			tempGeomAddIndices(&id[0], 12);
		} else if (_LineCap == LineCap::Round) {
			BX_CHECK(false, "Round caps not implemented for thin strokes.");
		}
	} else {
		BX_CHECK(firstSegmentLeftAAID != 0xFFFF && firstSegmentMiddleID != 0xFFFF && firstSegmentRightAAID != 0xFFFF, "Invalid first segment");

		uint16_t id[12] = {
			prevSegmentLeftAAID, prevSegmentMiddleID, firstSegmentMiddleID,
			prevSegmentLeftAAID, firstSegmentMiddleID, firstSegmentLeftAAID,
			prevSegmentMiddleID, prevSegmentRightAAID, firstSegmentRightAAID,
			prevSegmentMiddleID, firstSegmentRightAAID, firstSegmentMiddleID
		};

		tempGeomExpandIB(12);
		tempGeomAddIndices(&id[0], 12);
	}

	// Create a new draw command and copy the data to it.
	// TODO: Turn this into a function.
	const uint32_t numDrawVertices = m_TempGeomNumVertices;
	const uint32_t numDrawIndices = m_TempGeomNumIndices;
	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, m_TempGeomPos, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	memset64(dstUV, numDrawVertices, &uv.x);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	bx::memCopy(dstColor, m_TempGeomColor, sizeof(uint32_t) * numDrawVertices);

	const uint32_t startVertexID = cmd->m_NumVertices;
	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(m_TempGeomIndex, m_TempGeomNumIndices, dstIndex, (uint16_t)startVertexID);

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

template<bool _Closed, LineCap::Enum _LineCap, LineJoin::Enum _LineJoin>
void Context::renderPathStrokeNoAA(const Vec2* vtx, uint32_t numPathVertices, float strokeWidth, Color color)
{
	const Vec2 uv = getWhitePixelUV();
	const float avgScale = getState()->m_AvgScale;
	const uint32_t numSegments = numPathVertices - (_Closed ? 0 : 1);
	const float hsw = strokeWidth * 0.5f;
	const float da = bx::facos((avgScale * hsw) / ((avgScale * hsw) + m_TesselationTolerance)) * 2.0f;
	const uint32_t numPointsHalfCircle = bx::uint32_max(2u, (uint32_t)bx::fceil(bx::kPi / da));

	tempGeomReset();

	Vec2 d01;
	uint16_t prevSegmentLeftID = 0xFFFF;
	uint16_t prevSegmentRightID = 0xFFFF;
	uint16_t firstSegmentLeftID = 0xFFFF;
	uint16_t firstSegmentRightID = 0xFFFF;
	if (!_Closed) {
		// First segment of an open path
		const Vec2& p0 = vtx[0];
		const Vec2& p1 = vtx[1];

		d01 = p1 - p0;
		vec2Normalize(&d01);

		const Vec2 l01 = d01.perpCCW();

		if (_LineCap == LineCap::Butt) {
			const Vec2 l01_hsw = l01 * hsw;

			Vec2 p[2] = {
				p0 + l01_hsw,
				p0 - l01_hsw
			};

			tempGeomExpandVB(2);
			tempGeomAddPos(&p[0], 2);

			prevSegmentLeftID = 0;
			prevSegmentRightID = 1;
		} else if (_LineCap == LineCap::Square) {
			const Vec2 l01_hsw = l01 * hsw;
			const Vec2 d01_hsw = d01 * hsw;

			Vec2 p[2] = {
				p0 + l01_hsw - d01_hsw,
				p0 - l01_hsw - d01_hsw
			};

			tempGeomExpandVB(2);
			tempGeomAddPos(&p[0], 2);

			prevSegmentLeftID = 0;
			prevSegmentRightID = 1;
		} else if (_LineCap == LineCap::Round) {
			tempGeomExpandVB(numPointsHalfCircle);

			const float startAngle = atan2f(l01.y, l01.x);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle + i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p = Vec2(p0.x + ca * hsw, p0.y + sa * hsw);

				tempGeomAddPos(&p, 1);
			}

			tempGeomExpandIB((numPointsHalfCircle - 2) * 3);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				uint16_t id[3] = { 0, (uint16_t)(i + 1), (uint16_t)(i + 2) };
				tempGeomAddIndices(&id[0], 3);
			}

			prevSegmentLeftID = 0;
			prevSegmentRightID = (uint16_t)(numPointsHalfCircle - 1);
		} else {
			BX_CHECK(false, "Unknown line cap type");
		}
	} else {
		d01 = vtx[0] - vtx[numPathVertices - 1];
		vec2Normalize(&d01);
	}

	const uint32_t firstSegmentID = _Closed ? 0 : 1;
	for (uint32_t iSegment = firstSegmentID; iSegment < numSegments; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		Vec2 d12 = p2 - p1;
		vec2Normalize(&d12);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_hsw = v * hsw;

		// Check which one of the points is the inner corner.
		float leftPointProjDist = d12.x * v_hsw.x + d12.y * v_hsw.y;
		if (leftPointProjDist >= 0.0f) {
			// The left point is the inner corner.
			const Vec2 innerCorner = p1 + v_hsw;

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)m_TempGeomNumVertices;

				Vec2 p[2] = {
					innerCorner,
					p1 - v_hsw
				};

				tempGeomExpandVB(2);
				tempGeomAddPos(&p[0], 2);

				if (prevSegmentLeftID != 0xFFFF) {
					BX_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstVertexID + 1), firstVertexID
					};

					tempGeomExpandIB(6);
					tempGeomAddIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstVertexID; // 0
					firstSegmentRightID = firstVertexID + 1; // 1
				}

				prevSegmentLeftID = firstVertexID;
				prevSegmentRightID = firstVertexID + 1;
			} else {
				const Vec2 r01 = d01.perpCW();
				const Vec2 r12 = d12.perpCW();

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = atan2f(r01.y, r01.x);
					a12 = atan2f(r12.y, r12.x);
					if (a12 < a01) {
						a12 += bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a12 - a01) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				Vec2 p[3] = {
					innerCorner,
					p1 + r01 * hsw,
					p1 + r12 * hsw
				};

				uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;
				tempGeomExpandVB(numArcPoints + 2);
				tempGeomAddPos(&p[0], 2);
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;
					float ca = bx::fcos(a);
					float sa = bx::fsin(a);

					Vec2 p = Vec2(p1.x + hsw * ca, p1.y + hsw * sa);

					tempGeomAddPos(&p, 1);
				}
				tempGeomAddPos(&p[2], 1);

				if (prevSegmentLeftID != 0xFFFF) {
					BX_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, (uint16_t)(firstFanVertexID + 1),
						prevSegmentLeftID, (uint16_t)(firstFanVertexID + 1), firstFanVertexID
					};

					tempGeomExpandIB(6);
					tempGeomAddIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstFanVertexID; // 0
					firstSegmentRightID = firstFanVertexID + 1; // 1
				}

				// Generate the triangle fan.
				tempGeomExpandIB(numArcPoints * 3);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					const uint16_t idBase = firstFanVertexID + (uint16_t)iArcPoint;
					uint16_t id[3] = {
						firstFanVertexID, (uint16_t)(idBase + 1), (uint16_t)(idBase + 2)
					};
					tempGeomAddIndices(&id[0], 3);
				}

				prevSegmentLeftID = firstFanVertexID;
				prevSegmentRightID = firstFanVertexID + (uint16_t)numArcPoints + 1;
			}
		} else {
			// The right point is the inner corner.
			const Vec2 innerCorner = p1 - v_hsw;

			if (_LineJoin == LineJoin::Miter) {
				const uint16_t firstVertexID = (uint16_t)m_TempGeomNumVertices;

				Vec2 p[2] = {
					innerCorner,
					p1 + v_hsw,
				};

				tempGeomExpandVB(2);
				tempGeomAddPos(&p[0], 2);

				if (prevSegmentLeftID != 0xFFFF) {
					BX_CHECK(prevSegmentRightID != 0xFFFF, "Invalid previous segment");

					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, firstVertexID,
						prevSegmentLeftID, firstVertexID, (uint16_t)(firstVertexID + 1)
					};

					tempGeomExpandIB(6);
					tempGeomAddIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstVertexID + 1; // 1
					firstSegmentRightID = firstVertexID; // 0
				}

				prevSegmentLeftID = firstVertexID + 1;
				prevSegmentRightID = firstVertexID;
			} else {
				const Vec2 l01 = d01.perpCCW();
				const Vec2 l12 = d12.perpCCW();

				// Assume _LineJoin == LineJoin::Bevel
				float a01 = 0.0f, a12 = 0.0f, arcDa = 0.0f;
				uint32_t numArcPoints = 1;
				if (_LineJoin == LineJoin::Round) {
					a01 = atan2f(l01.y, l01.x);
					a12 = atan2f(l12.y, l12.x);
					if (a12 > a01) {
						a12 -= bx::kPi2;
					}

					numArcPoints = bx::uint32_max(2u, (uint32_t)((a01 - a12) / da));
					arcDa = ((a12 - a01) / (float)numArcPoints);
				}

				Vec2 p[3] = {
					innerCorner,
					p1 + l01 * hsw,
					p1 + l12 * hsw
				};

				uint16_t firstFanVertexID = (uint16_t)m_TempGeomNumVertices;
				tempGeomExpandVB(numArcPoints + 2);
				tempGeomAddPos(&p[0], 2);
				for (uint32_t iArcPoint = 1; iArcPoint < numArcPoints; ++iArcPoint) {
					float a = a01 + iArcPoint * arcDa;
					float ca = bx::fcos(a);
					float sa = bx::fsin(a);

					Vec2 p = Vec2(p1.x + hsw * ca, p1.y + hsw * sa);
					tempGeomAddPos(&p, 1);
				}
				tempGeomAddPos(&p[2], 1);

				if (prevSegmentLeftID != 0xFFFF && prevSegmentRightID != 0xFFFF) {
					uint16_t id[6] = {
						prevSegmentLeftID, prevSegmentRightID, firstFanVertexID,
						prevSegmentLeftID, firstFanVertexID, (uint16_t)(firstFanVertexID + 1)
					};

					tempGeomExpandIB(6);
					tempGeomAddIndices(&id[0], 6);
				} else {
					firstSegmentLeftID = firstFanVertexID + 1; // 1
					firstSegmentRightID = firstFanVertexID; // 0
				}

				tempGeomExpandIB(numArcPoints * 3);
				for (uint32_t iArcPoint = 0; iArcPoint < numArcPoints; ++iArcPoint) {
					const uint16_t idBase = firstFanVertexID + (uint16_t)iArcPoint;
					uint16_t id[3] = {
						firstFanVertexID, (uint16_t)(idBase + 2), (uint16_t)(idBase + 1)
					};
					tempGeomAddIndices(&id[0], 3);
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

		const Vec2 l01 = d01.perpCCW();

		if (_LineCap == LineCap::Butt) {
			const uint16_t curSegmentLeftID = (uint16_t)m_TempGeomNumVertices;
			const Vec2 l01_hsw = l01 * hsw;

			Vec2 p[2] = {
				p1 + l01_hsw,
				p1 - l01_hsw
			};

			tempGeomExpandVB(2);
			tempGeomAddPos(&p[0], 2);

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + 1), curSegmentLeftID
			};

			tempGeomExpandIB(6);
			tempGeomAddIndices(&id[0], 6);
		} else if (_LineCap == LineCap::Square) {
			const uint16_t curSegmentLeftID = (uint16_t)m_TempGeomNumVertices;
			const Vec2 l01_hsw = l01 * hsw;
			const Vec2 d01_hsw = d01 * hsw;

			Vec2 p[2] = {
				p1 + l01_hsw + d01_hsw,
				p1 - l01_hsw + d01_hsw
			};

			tempGeomExpandVB(2);
			tempGeomAddPos(&p[0], 2);

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + 1),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + 1), curSegmentLeftID
			};

			tempGeomExpandIB(6);
			tempGeomAddIndices(&id[0], 6);
		} else if (_LineCap == LineCap::Round) {
			tempGeomExpandVB(numPointsHalfCircle);

			const uint16_t curSegmentLeftID = (uint16_t)m_TempGeomNumVertices;
			const float startAngle = atan2f(l01.y, l01.x);
			for (uint32_t i = 0; i < numPointsHalfCircle; ++i) {
				float a = startAngle - i * bx::kPi / (float)(numPointsHalfCircle - 1);
				float ca = bx::fcos(a);
				float sa = bx::fsin(a);

				Vec2 p = Vec2(p1.x + ca * hsw, p1.y + sa * hsw);

				tempGeomAddPos(&p, 1);
			}

			uint16_t id[6] = {
				prevSegmentLeftID, prevSegmentRightID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1)),
				prevSegmentLeftID, (uint16_t)(curSegmentLeftID + (numPointsHalfCircle - 1)), curSegmentLeftID
			};

			tempGeomExpandIB(6 + (numPointsHalfCircle - 2) * 3);
			tempGeomAddIndices(&id[0], 6);
			for (uint32_t i = 0; i < numPointsHalfCircle - 2; ++i) {
				const uint16_t idBase = curSegmentLeftID + (uint16_t)i;
				uint16_t id[3] = {
					curSegmentLeftID, (uint16_t)(idBase + 2), (uint16_t)(idBase + 1)
				};
				tempGeomAddIndices(&id[0], 3);
			}
		}
	} else {
		// Generate the first segment quad. 
		uint16_t id[6] = {
			prevSegmentLeftID, prevSegmentRightID, firstSegmentRightID,
			prevSegmentLeftID, firstSegmentRightID, firstSegmentLeftID
		};

		tempGeomExpandIB(6);
		tempGeomAddIndices(&id[0], 6);
	}

	// Create a new draw command and copy the data to it.
	const uint32_t numDrawVertices = m_TempGeomNumVertices;
	const uint32_t numDrawIndices = m_TempGeomNumIndices;
	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, m_TempGeomPos, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	memset64(dstUV, numDrawVertices, &uv.x);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numDrawVertices, &color);

	const uint32_t startVertexID = cmd->m_NumVertices;
	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(m_TempGeomIndex, m_TempGeomNumIndices, dstIndex, (uint16_t)startVertexID);

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

Shape* Context::createShape(uint32_t flags)
{
	bx::MemoryBlock* memBlock = BX_NEW(m_Allocator, bx::MemoryBlock)(m_Allocator);
	Shape* shape = BX_NEW(m_Allocator, Shape)(memBlock);

	shape->m_Flags = flags & (ShapeFlag::AllowCommandReordering | ShapeFlag::EnableCaching);

	// TODO: Keep the shape ptr to make sure we aren't leaking any memory 
	// even if the owner of the shape forgets to destroy it.

	return shape;
}

void Context::destroyShape(Shape* shape)
{
	if (shape->m_RendererData) {
		CachedShape* cachedShape = (CachedShape*)shape->m_RendererData;
		BX_DELETE(m_Allocator, cachedShape);
		shape->m_RendererData = nullptr;
	}

	BX_DELETE(m_Allocator, (bx::MemoryBlock*)shape->m_CmdList);
	shape->m_CmdList = nullptr;

	BX_DELETE(m_Allocator, shape);
}

void Context::submitShape(Shape* shape, GetStringByIDFunc* stringCallback, void* userData)
{
#if VG_CONFIG_ENABLE_SHAPE_CACHING
	const State* state = getState();

	// TODO: Currently only the shapes with solid color draw commands can be cached (no gradients and no images).
	bool canCache = (shape->m_Flags & (ShapeFlag::HasGradients | ShapeFlag::HasImages)) == 0;

	// If there are text commands in the shape, allow caching only if command reordering is allowed (render text
	// after geometry, in a separate loop).
	if (canCache && (shape->m_Flags & (ShapeFlag::HasText | ShapeFlag::HasDynamicText))) {
		canCache = (shape->m_Flags & ShapeFlag::AllowCommandReordering) != 0;
	}

	if (canCache) {
		canCache = (shape->m_Flags & ShapeFlag::EnableCaching) != 0;
	}

	CachedShape* cachedShape = canCache ? (CachedShape*)shape->m_RendererData : nullptr;
	if (cachedShape && cachedShape->m_AvgScale == state->m_AvgScale) {
		cachedShapeRender(cachedShape, stringCallback, userData);
		return;
	}

	// Either there's no cached version of this shape or the scaling has changed.
	if (canCache) {
		if (cachedShape) {
			cachedShapeReset(cachedShape);
		} else {
			cachedShape = BX_NEW(m_Allocator, CachedShape)();
			shape->m_RendererData = cachedShape;
		}

		cachedShape->m_AvgScale = state->m_AvgScale;
		invertMatrix3(state->m_TransformMtx, cachedShape->m_InvTransformMtx);
	} else {
		if (cachedShape) {
			// This shape used to be cachable but now it's not (user reset the EnableCache flag).
			// Destroy the cached shape.
			cachedShapeReset(cachedShape);
			BX_DELETE(m_Allocator, cachedShape);
			shape->m_RendererData = nullptr;
			cachedShape = nullptr;
		}
	}

	// Now execute the command stream and keep every new DrawCommand in the cachedShape.
#endif // VG_CONFIG_ENABLE_SHAPE_CACHING

	const uint8_t* cmdList = (uint8_t*)shape->m_CmdList->more(0);
	const uint32_t cmdListSize = shape->m_CmdList->getSize();

	const uint8_t* cmdListEnd = cmdList + cmdListSize;

	const uint16_t firstGradientID = (uint16_t)m_NextGradientID;
	const uint16_t firstImagePatternID = (uint16_t)m_NextImagePatternID;
	BX_CHECK(firstGradientID + shape->m_NumGradients <= VG_CONFIG_MAX_GRADIENTS, "Not enough free gradients to render shape. Increase VG_MAX_GRADIENTS");
	BX_CHECK(firstImagePatternID + shape->m_NumImagePatterns <= VG_CONFIG_MAX_IMAGE_PATTERNS, "Not enough free image patterns to render shape. Increase VG_MAX_IMAGE_PATTERS");

	while (cmdList < cmdListEnd) {
		ShapeCommand::Enum cmdType = *(ShapeCommand::Enum*)cmdList;
		cmdList += sizeof(ShapeCommand::Enum);

		switch (cmdType) {
		case ShapeCommand::BeginPath:
		{
			beginPath();
			break;
		}
		case ShapeCommand::ClosePath:
		{
			closePath();
			break;
		}
		case ShapeCommand::MoveTo:
		{
			float* coords = (float*)cmdList;
			moveTo(coords[0], coords[1]);
			cmdList += sizeof(float) * 2;
			break;
		}
		case ShapeCommand::LineTo:
		{
			float* coords = (float*)cmdList;
			lineTo(coords[0], coords[1]);
			cmdList += sizeof(float) * 2;
			break;
		}
		case ShapeCommand::BezierTo:
		{
			float* coords = (float*)cmdList;
			bezierTo(coords[0], coords[1], coords[2], coords[3], coords[4], coords[5]);
			cmdList += sizeof(float) * 6;
			break;
		}
		case ShapeCommand::ArcTo:
		{
			float* coords = (float*)cmdList;
			arcTo(coords[0], coords[1], coords[2], coords[3], coords[4]);
			cmdList += sizeof(float) * 5;
			break;
		}
		case ShapeCommand::Rect:
		{
			float* coords = (float*)cmdList;
			rect(coords[0], coords[1], coords[2], coords[3]);
			cmdList += sizeof(float) * 4;
			break;
		}
		case ShapeCommand::RoundedRect:
		{
			float* coords = (float*)cmdList;
			roundedRect(coords[0], coords[1], coords[2], coords[3], coords[4]);
			cmdList += sizeof(float) * 5;
			break;
		}
		case ShapeCommand::Circle:
		{
			float* coords = (float*)cmdList;
			circle(coords[0], coords[1], coords[2]);
			cmdList += sizeof(float) * 3;
			break;
		}
		case ShapeCommand::FillConvexColor:
		{
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				// Force a new draw command to easily determine which triangles 
				// have been generated for this call.
				m_ForceNewDrawCommand = true;
			}

			// Keep the current DrawCommand, just in case this call doesn't 
			// generate any triangles.
			const uint32_t prevDrawCommandID = m_NumDrawCommands - 1;
#endif

			Color col = CMD_READ(Color, cmdList);
			bool aa = CMD_READ(bool, cmdList);
			fillConvexPath(col, aa);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				if (prevDrawCommandID != m_NumDrawCommands - 1) {
					cachedShapeAddDrawCommand(cachedShape, &m_DrawCommands[m_NumDrawCommands - 1]);
				}
			}
#endif
			break;
		}
		case ShapeCommand::FillConvexGradient:
		{
			// TODO: Cache gradient fills.
			GradientHandle handle = CMD_READ(GradientHandle, cmdList);
			bool aa = CMD_READ(bool, cmdList);

			handle.idx += firstGradientID;
			fillConvexPath(handle, aa);
			break;
		}
		case ShapeCommand::FillConvexImage:
		{
			// TODO: Cache image fills
			ImagePatternHandle handle = CMD_READ(ImagePatternHandle, cmdList);
			bool aa = CMD_READ(bool, cmdList);

			handle.idx += firstImagePatternID;
			fillConvexPath(handle, aa);
			break;
		}
		case ShapeCommand::FillConcaveColor:
		{
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				m_ForceNewDrawCommand = true;
			}

			const uint32_t prevDrawCommandID = m_NumDrawCommands - 1;
#endif

			Color col = CMD_READ(Color, cmdList);
			bool aa = CMD_READ(bool, cmdList);
			fillConcavePath(col, aa);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				if (prevDrawCommandID != m_NumDrawCommands - 1) {
					// TODO: Assume only 1 (batched) draw command will generated for this path.
					// Since this is a concave polygon, it will be broken up into at least 2 convex
					// parts. There's a chance the first convex part ends up in the current VB but the
					// the rest of the parts are placed in a new VB (can happen if the VB is nearly full).
					// Loop over all commands and add them to the CachedShape.
					cachedShapeAddDrawCommand(cachedShape, &m_DrawCommands[m_NumDrawCommands - 1]);
				}
			}
#endif
			break;
		}
		case ShapeCommand::Stroke:
		{
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				m_ForceNewDrawCommand = true;
			}

			const uint32_t prevDrawCommandID = m_NumDrawCommands - 1;
#endif

			Color col = CMD_READ(Color, cmdList);
			float width = CMD_READ(float, cmdList);
			bool aa = CMD_READ(bool, cmdList);
			LineCap::Enum lineCap = CMD_READ(LineCap::Enum, cmdList);
			LineJoin::Enum lineJoin = CMD_READ(LineJoin::Enum, cmdList);
			strokePath(col, width, aa, lineCap, lineJoin);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				if (prevDrawCommandID != m_NumDrawCommands - 1) {
					cachedShapeAddDrawCommand(cachedShape, &m_DrawCommands[m_NumDrawCommands - 1]);
				}
			}
#endif
			break;
		}
		case ShapeCommand::LinearGradient:
		{
			float sx = CMD_READ(float, cmdList);
			float sy = CMD_READ(float, cmdList);
			float ex = CMD_READ(float, cmdList);
			float ey = CMD_READ(float, cmdList);
			Color icol = CMD_READ(Color, cmdList);
			Color ocol = CMD_READ(Color, cmdList);
			createLinearGradient(sx, sy, ex, ey, icol, ocol);
			break;
		}
		case ShapeCommand::BoxGradient:
		{
			float x = CMD_READ(float, cmdList);
			float y = CMD_READ(float, cmdList);
			float w = CMD_READ(float, cmdList);
			float h = CMD_READ(float, cmdList);
			float r = CMD_READ(float, cmdList);
			float f = CMD_READ(float, cmdList);
			Color icol = CMD_READ(Color, cmdList);
			Color ocol = CMD_READ(Color, cmdList);
			createBoxGradient(x, y, w, h, r, f, icol, ocol);
			break;
		}
		case ShapeCommand::RadialGradient:
		{
			float cx = CMD_READ(float, cmdList);
			float cy = CMD_READ(float, cmdList);
			float inr = CMD_READ(float, cmdList);
			float outr = CMD_READ(float, cmdList);
			Color icol = CMD_READ(Color, cmdList);
			Color ocol = CMD_READ(Color, cmdList);
			createRadialGradient(cx, cy, inr, outr, icol, ocol);
			break;
		}
		case ShapeCommand::ImagePattern:
		{
			float cx = CMD_READ(float, cmdList);
			float cy = CMD_READ(float, cmdList);
			float w = CMD_READ(float, cmdList);
			float h = CMD_READ(float, cmdList);
			float angle = CMD_READ(float, cmdList);
			ImageHandle image = CMD_READ(ImageHandle, cmdList);
			float alpha = CMD_READ(float, cmdList);
			createImagePattern(cx, cy, w, h, angle, image, alpha);
			break;
		}
		case ShapeCommand::TextStatic:
		{
//#if VG_CONFIG_ENABLE_SHAPE_CACHING
//			if (canCache) {
//				cachedShapeAddTextCommand(cachedShape, cmdList - sizeof(ShapeCommand::Enum));
//			}
//#endif

			Font font = CMD_READ(Font, cmdList);
			uint32_t alignment = CMD_READ(uint32_t, cmdList);
			Color col = CMD_READ(Color, cmdList);
			float x = CMD_READ(float, cmdList);
			float y = CMD_READ(float, cmdList);
			uint32_t len = CMD_READ(uint32_t, cmdList);
			const char* str = (const char*)cmdList;
			cmdList += len;

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			String* cachedString = createString(font, str, str + len);
			cachedShapeAddTextCommand(cachedShape, cachedString, alignment, col, x, y);
			text(cachedString, alignment, col, x, y);
#else
			text(font, alignment, col, x, y, str, str + len);
#endif
			break;
		}
#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
		case ShapeCommand::TextDynamic:
		{
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				cachedShapeAddTextCommand(cachedShape, cmdList - sizeof(ShapeCommand::Enum));
			}
#endif

			Font font = CMD_READ(Font, cmdList);
			uint32_t alignment = CMD_READ(uint32_t, cmdList);
			Color col = CMD_READ(Color, cmdList);
			float x = CMD_READ(float, cmdList);
			float y = CMD_READ(float, cmdList);
			uint32_t stringID = CMD_READ(uint32_t, cmdList);

			BX_WARN(stringCallback, "Shape includes dynamic text commands but no string callback has been specified");
			if (stringCallback) {
				uint32_t len;
				const char* str = (*stringCallback)(stringID, len, userData);
				const char* end = len != ~0u ? str + len : str + bx::strLen(str);

				text(font, alignment, col, x, y, str, end);
				break;
			}
		}
#endif
		}
	}

	// Free shape gradients and image patterns
	m_NextGradientID = firstGradientID;
	m_NextImagePatternID = firstImagePatternID;
}

void Context::cachedShapeRender(const CachedShape* shape, GetStringByIDFunc* stringCallback, void* userData)
{
	const State* state = getState();
	const float* stateMtx = state->m_TransformMtx;

	float mtx[6];
	multiplyMatrix3(stateMtx, shape->m_InvTransformMtx, mtx);

	const uint32_t numCommands = shape->m_NumDrawCommands;
	for (uint32_t iCmd = 0; iCmd < numCommands; ++iCmd) {
		const CachedDrawCommand* cachedCmd = &shape->m_DrawCommands[iCmd];

		DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(cachedCmd->m_NumVertices, cachedCmd->m_NumIndices, m_FontImages[0]);

		VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
		const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;
		Vec2* dstPos = &vb->m_Pos[vbOffset];
		Vec2* dstUV = &vb->m_UV[vbOffset];
		uint32_t* dstColor = &vb->m_Color[vbOffset];
		uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

		const uint16_t startVertexID = (uint16_t)cmd->m_NumVertices;

		batchTransformPositions_Unaligned(cachedCmd->m_Pos, cachedCmd->m_NumVertices, dstPos, mtx);
		bx::memCopy(dstUV, cachedCmd->m_UV, sizeof(Vec2) * cachedCmd->m_NumVertices);
		bx::memCopy(dstColor, cachedCmd->m_Color, sizeof(uint32_t) * cachedCmd->m_NumVertices);
		batchTransformDrawIndices(cachedCmd->m_Indices, cachedCmd->m_NumIndices, dstIndex, startVertexID);

		cmd->m_NumVertices += cachedCmd->m_NumVertices;
		cmd->m_NumIndices += cachedCmd->m_NumIndices;
	}

	// Render all text commands...
	const uint32_t numStaticTextCommands = shape->m_NumStaticTextCommands;
	for (uint32_t iStatic = 0; iStatic < numStaticTextCommands; ++iStatic) {
		CachedTextCommand* cmd = &shape->m_StaticTextCommands[iStatic];
		text(cmd->m_Text, cmd->m_Alignment, cmd->m_Color, cmd->m_Pos.x, cmd->m_Pos.y);
	}

	const uint32_t numTextCommands = shape->m_NumTextCommands;
	for (uint32_t iText = 0; iText < numTextCommands; ++iText) {
		const uint8_t* cmdData = shape->m_TextCommands[iText];

		ShapeCommand::Enum cmdType = *(ShapeCommand::Enum*)cmdData;
		cmdData += sizeof(ShapeCommand::Enum);

		switch (cmdType) {
		case ShapeCommand::TextStatic:
		{
			Font font = CMD_READ(Font, cmdData);
			uint32_t alignment = CMD_READ(uint32_t, cmdData);
			Color col = CMD_READ(Color, cmdData);
			float x = CMD_READ(float, cmdData);
			float y = CMD_READ(float, cmdData);
			uint32_t len = CMD_READ(uint32_t, cmdData);
			const char* str = (const char*)cmdData;
			cmdData += len;
			text(font, alignment, col, x, y, str, str + len);
			break;
		}
#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
		case ShapeCommand::TextDynamic:
		{
			Font font = CMD_READ(Font, cmdData);
			uint32_t alignment = CMD_READ(uint32_t, cmdData);
			Color col = CMD_READ(Color, cmdData);
			float x = CMD_READ(float, cmdData);
			float y = CMD_READ(float, cmdData);
			uint32_t stringID = CMD_READ(uint32_t, cmdData);

			BX_WARN(stringCallback, "Shape includes dynamic text commands but not string callback has been specified");
			if (stringCallback) {
				uint32_t len;
				const char* str = (*stringCallback)(stringID, len, userData);
				const char* end = len != ~0u ? str + len : str + bx::strLen(str);

				text(font, alignment, col, x, y, str, end);
				break;
			}
		}
#endif
		default:
			BX_CHECK(false, "Unknown shape command");
		}
	}
}

void Context::cachedShapeReset(CachedShape* shape)
{
	for (uint32_t i = 0; i < shape->m_NumDrawCommands; ++i) {
		BX_ALIGNED_FREE(m_Allocator, shape->m_DrawCommands[i].m_Pos, 16);
		BX_ALIGNED_FREE(m_Allocator, shape->m_DrawCommands[i].m_UV, 16);
		BX_ALIGNED_FREE(m_Allocator, shape->m_DrawCommands[i].m_Color, 16);
		BX_ALIGNED_FREE(m_Allocator, shape->m_DrawCommands[i].m_Indices, 16);
	}
	BX_FREE(m_Allocator, shape->m_DrawCommands);
	shape->m_DrawCommands = nullptr;
	shape->m_NumDrawCommands = 0;

	for (uint32_t i = 0; i < shape->m_NumStaticTextCommands; ++i) {
		destroyString(shape->m_StaticTextCommands[i].m_Text);
	}
	BX_FREE(m_Allocator, shape->m_StaticTextCommands);
	shape->m_StaticTextCommands = nullptr;
	shape->m_NumStaticTextCommands = 0;

	BX_FREE(m_Allocator, shape->m_TextCommands);
	shape->m_TextCommands = nullptr;
	shape->m_NumTextCommands = 0;
}

void Context::cachedShapeAddDrawCommand(CachedShape* shape, const DrawCommand* cmd)
{
	BX_CHECK(cmd->m_NumVertices < 65536, "Each draw command should have max 65535 vertices");
	BX_CHECK(cmd->m_NumIndices < 65536, "Each draw command should have max 65535 indices");
	BX_CHECK(cmd->m_Type == DrawCommand::Type_TexturedVertexColor && cmd->m_HandleID == m_FontImages[0].idx, "Cannot cache draw command");

	// TODO: Currently only solid color untextured paths are cached. So all draw commands can be batched into a single VB/IB.
	CachedDrawCommand* cachedCmd = nullptr;
	if (shape->m_NumDrawCommands < 1) {
		shape->m_NumDrawCommands++;
		shape->m_DrawCommands = (CachedDrawCommand*)BX_REALLOC(m_Allocator, shape->m_DrawCommands, sizeof(CachedDrawCommand) * shape->m_NumDrawCommands);

		cachedCmd = &shape->m_DrawCommands[0];
		cachedCmd->m_Indices = nullptr;
		cachedCmd->m_Pos = nullptr;
		cachedCmd->m_UV = nullptr;
		cachedCmd->m_Color = nullptr;
		cachedCmd->m_NumVertices = 0;
		cachedCmd->m_NumIndices = 0;
	} else {
		cachedCmd = &shape->m_DrawCommands[shape->m_NumDrawCommands - 1];
	}

	const uint16_t firstVertexID = cachedCmd->m_NumVertices;
	const uint16_t firstIndexID = cachedCmd->m_NumIndices;

	BX_CHECK(cachedCmd->m_NumVertices + cmd->m_NumVertices < 65536, "Not enough space in current cached command");
	BX_CHECK(cachedCmd->m_NumIndices + cmd->m_NumIndices < 65536, "Not enough space in current cached command");
	cachedCmd->m_NumVertices += (uint16_t)cmd->m_NumVertices;
	cachedCmd->m_Pos = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_Pos, sizeof(Vec2) * cachedCmd->m_NumVertices, 16);
	cachedCmd->m_UV = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_UV, sizeof(Vec2) * cachedCmd->m_NumVertices, 16);
	cachedCmd->m_Color = (uint32_t*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_Color, sizeof(uint32_t) * cachedCmd->m_NumVertices, 16);

	const Vec2* srcPos = &m_VertexBuffers[cmd->m_VertexBufferID].m_Pos[cmd->m_FirstVertexID];
	const Vec2* srcUV = &m_VertexBuffers[cmd->m_VertexBufferID].m_UV[cmd->m_FirstVertexID];
	const uint32_t* srcColor = &m_VertexBuffers[cmd->m_VertexBufferID].m_Color[cmd->m_FirstVertexID];
	bx::memCopy(&cachedCmd->m_Pos[firstVertexID], srcPos, sizeof(Vec2) * cmd->m_NumVertices);
	bx::memCopy(&cachedCmd->m_UV[firstVertexID], srcUV, sizeof(Vec2) * cmd->m_NumVertices);
	bx::memCopy(&cachedCmd->m_Color[firstVertexID], srcColor, sizeof(uint32_t) * cmd->m_NumVertices);

	// Copy the indices...
	cachedCmd->m_NumIndices += (uint16_t)cmd->m_NumIndices;
	cachedCmd->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_Indices, sizeof(uint16_t) * cachedCmd->m_NumIndices, 16);

	const uint16_t* srcIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID];
	uint16_t* dstIndex = &cachedCmd->m_Indices[firstIndexID];
	const uint32_t numIndices = cmd->m_NumIndices;
	for (uint32_t i = 0; i < numIndices; ++i) {
		*dstIndex++ = *srcIndex++ + firstVertexID;
	}
}

void Context::cachedShapeAddTextCommand(CachedShape* shape, const uint8_t* cmdData)
{
	shape->m_NumTextCommands++;
	shape->m_TextCommands = (const uint8_t**)BX_REALLOC(m_Allocator, shape->m_TextCommands, sizeof(uint8_t*) * shape->m_NumTextCommands);
	shape->m_TextCommands[shape->m_NumTextCommands - 1] = cmdData;
}

void Context::cachedShapeAddTextCommand(CachedShape* shape, String* str, uint32_t alignment, Color col, float x, float y)
{
	shape->m_NumStaticTextCommands++;
	shape->m_StaticTextCommands = (CachedTextCommand*)BX_REALLOC(m_Allocator, shape->m_StaticTextCommands, sizeof(CachedTextCommand) * shape->m_NumStaticTextCommands);

	CachedTextCommand* cmd = &shape->m_StaticTextCommands[shape->m_NumStaticTextCommands - 1];
	cmd->m_Text = str;
	cmd->m_Alignment = alignment;
	cmd->m_Color = col;
	cmd->m_Pos.x = x;
	cmd->m_Pos.y = y;
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
		BX_CHECK(iw != -1 && ih != -1, "Invalid font atlas dimensions");

		if (iw > ih) {
			ih *= 2;
		} else {
			iw *= 2;
		}

		const int maxTextureSize = (int)bgfx::getCaps()->limits.maxTextureSize;
		if (iw > maxTextureSize || ih > maxTextureSize) {
			iw = ih = maxTextureSize;
		}

		m_FontImages[m_FontImageID + 1] = createImageRGBA((uint16_t)iw, (uint16_t)ih, ImageFlags::Filter_Bilinear, nullptr);
	}

	++m_FontImageID;

	fonsResetAtlas(m_FontStashContext, iw, ih);

	return true;
}

void Context::renderTextQuads(uint32_t numQuads, Color color)
{
	State* state = getState();
	float scale = state->m_FontScale * m_DevicePixelRatio;
	float invscale = 1.0f / scale;

	const uint32_t c = ColorRGBA::setAlpha(color, (uint8_t)(state->m_GlobalAlpha * ColorRGBA::getAlpha(color)));
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	float mtx[6];
	mtx[0] = state->m_TransformMtx[0] * invscale;
	mtx[1] = state->m_TransformMtx[1] * invscale;
	mtx[2] = state->m_TransformMtx[2] * invscale;
	mtx[3] = state->m_TransformMtx[3] * invscale;
	mtx[4] = state->m_TransformMtx[4];
	mtx[5] = state->m_TransformMtx[5];

	// TODO: Calculate bounding rect of the quads.
	batchTransformTextQuads(m_TextQuads, numQuads, mtx, m_TextVertices);

	const uint32_t numDrawVertices = numQuads * 4;
	const uint32_t numDrawIndices = numQuads * 6;

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[m_FontImageID]);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, m_TextVertices, sizeof(Vec2) * numDrawVertices);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numDrawVertices, &c);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	const FONSquad* q = m_TextQuads;
	uint32_t nq = numQuads;
	while (nq-- > 0) {
		const float s0 = q->s0;
		const float s1 = q->s1;
		const float t0 = q->t0;
		const float t1 = q->t1;

		dstUV[0].x = s0; dstUV[0].y = t0;
		dstUV[1].x = s1; dstUV[1].y = t0;
		dstUV[2].x = s1; dstUV[2].y = t1;
		dstUV[3].x = s0; dstUV[3].y = t1;

		dstUV += 4;
		++q;
	}

	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	uint32_t startVertexID = cmd->m_NumVertices;
	while (numQuads-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)(startVertexID + 1);
		*dstIndex++ = (uint16_t)(startVertexID + 2);
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)(startVertexID + 2);
		*dstIndex++ = (uint16_t)(startVertexID + 3);

		startVertexID += 4;
	}

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
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

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, vtx, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	memset64(dstUV, numDrawVertices, &uv.x);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numDrawVertices, &c);

	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	const uint32_t startVertexID = cmd->m_NumVertices;
	uint32_t secondTriVertex = startVertexID + 1;
	while (numTris-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 1);
		++secondTriVertex;
	}

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
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

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstUV = &vb->m_UV[vbOffset];
	memset64(dstUV, numDrawVertices, &uv.x);

	const uint32_t colors[2] = { c, c0 };
	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset64(dstColor, numDrawVertices >> 1, &colors[0]);

	// TODO: Assumes CCW order (otherwise fringes are generated on the wrong side of the polygon)
	Vec2* dstPos = &vb->m_Pos[vbOffset];
	Vec2 d01 = vtx[0] - vtx[numPathVertices - 1];
	vec2Normalize(&d01);

	for (uint32_t iSegment = 0; iSegment < numPathVertices; ++iSegment) {
		const Vec2& p1 = vtx[iSegment];
		const Vec2& p2 = vtx[iSegment == numPathVertices - 1 ? 0 : iSegment + 1];

		Vec2 d12 = p2 - p1;
		vec2Normalize(&d12);

		const Vec2 v = calcExtrusionVector(d01, d12);
		const Vec2 v_aa = v * aa;

		dstPos[0] = p1 - v_aa;
		dstPos[1] = p1 + v_aa;
		dstPos += 2;

		d01 = d12;
	}

	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	const uint32_t startVertexID = cmd->m_NumVertices;

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

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

void Context::renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numPathVertices, ImagePatternHandle handle)
{
	BX_CHECK(isValid(handle), "Invalid image pattern handle");

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

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, vtx, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	batchTransformPositions_Unaligned(vtx, numDrawVertices, dstUV, mtx);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numDrawVertices, &c);

	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	const uint32_t startVertexID = cmd->m_NumVertices;
	uint32_t secondTriVertex = startVertexID + 1;
	while (numTris-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 1);
		++secondTriVertex;
	}

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

void Context::renderConvexPolygonNoAA(const Vec2* vtx, uint32_t numPathVertices, GradientHandle handle)
{
	BX_CHECK(isValid(handle), "Invalid gradient handle");

	uint32_t numTris = numPathVertices - 2;
	uint32_t numDrawVertices = numPathVertices;
	uint32_t numDrawIndices = numTris * 3; // N - 2 triangles in a N pt fan, 3 indices per triangle

	DrawCommand* cmd = allocDrawCommand_ColorGradient(numDrawVertices, numDrawIndices, handle);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;
	uint32_t* dstColor = &vb->m_Color[vbOffset];

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, vtx, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	const float uv[2] = { 0.0f, 0.0f };
	memset64(dstUV, numDrawVertices, &uv[0]);

	const uint32_t color = ColorRGBA::White;
	memset32(dstColor, numDrawVertices, &color);

	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	const uint32_t startVertexID = cmd->m_NumVertices;
	uint32_t secondTriVertex = startVertexID + 1;
	while (numTris-- > 0) {
		*dstIndex++ = (uint16_t)startVertexID;
		*dstIndex++ = (uint16_t)secondTriVertex;
		*dstIndex++ = (uint16_t)(secondTriVertex + 1);
		++secondTriVertex;
	}

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

DrawCommand* Context::allocDrawCommand_TexturedVertexColor(uint32_t numVertices, uint32_t numIndices, ImageHandle img)
{
	BX_CHECK(isValid(img), "Invalid image handle");

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
		ib->m_Capacity = bx::uint32_max(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity, 16);
	}

	if (!m_ForceNewDrawCommand && m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &m_DrawCommands[m_NumDrawCommands - 1];

		BX_CHECK(prevCmd->m_VertexBufferID == vbID, "Cannot merge draw commands with different vertex buffers");
		BX_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] 
			&& prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] 
			&& prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] 
			&& prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");

		if (prevCmd->m_Type == DrawCommand::Type_TexturedVertexColor && 
			prevCmd->m_HandleID == img.idx) 
		{
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
	cmd->m_FirstIndexID = ib->m_Count;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type_TexturedVertexColor;
	cmd->m_HandleID = img.idx;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];

	vb->m_Count += numVertices;
	ib->m_Count += numIndices;

	m_ForceNewDrawCommand = false;

	return cmd;
}

DrawCommand* Context::allocDrawCommand_ColorGradient(uint32_t numVertices, uint32_t numIndices, GradientHandle gradientHandle)
{
	BX_CHECK(isValid(gradientHandle), "Invalid gradient handle");

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
		ib->m_Capacity = bx::uint32_max(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity, 16);
	}

	if (!m_ForceNewDrawCommand && m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &m_DrawCommands[m_NumDrawCommands - 1];

		BX_CHECK(prevCmd->m_VertexBufferID == vbID, "Cannot merge draw commands with different vertex buffers");
		BX_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] 
			&& prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1] 
			&& prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2] 
			&& prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");

		if (prevCmd->m_Type == DrawCommand::Type_ColorGradient &&
			prevCmd->m_HandleID == gradientHandle.idx) 
		{
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
	cmd->m_FirstIndexID = ib->m_Count;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type_ColorGradient;
	cmd->m_HandleID = gradientHandle.idx;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];

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
		
		VertexBuffer* vb = &m_VertexBuffers[m_VertexBufferCapacity - 1];

		vb->m_PosBufferHandle = BGFX_INVALID_HANDLE;
		vb->m_UVBufferHandle = BGFX_INVALID_HANDLE;
		vb->m_ColorBufferHandle = BGFX_INVALID_HANDLE;
	}

	VertexBuffer* vb = &m_VertexBuffers[m_NumVertexBuffers++];
	vb->m_Pos = allocVertexBufferData_Vec2();
	vb->m_UV = allocVertexBufferData_Vec2();
	vb->m_Color = allocVertexBufferData_Uint32();
	vb->m_Count = 0;

	return vb;
}

ImageHandle Context::createImageRGBA(uint16_t w, uint16_t h, uint32_t flags, const uint8_t* data)
{
	ImageHandle handle = allocImage();
	if (!isValid(handle)) {
		return handle;
	}

	Image* tex = &m_Images[handle.idx];
	tex->m_Width = w;
	tex->m_Height = h;

	uint32_t bgfxFlags = BGFX_TEXTURE_NONE;

#if BX_PLATFORM_EMSCRIPTEN
	if (!isPowerOf2(w) || !isPowerOf2(h)) {
		flags = ImageFlags::Filter_NearestUV | ImageFlags::Filter_NearestW;
		bgfxFlags |= BGFX_TEXTURE_U_CLAMP | BGFX_TEXTURE_V_CLAMP | BGFX_TEXTURE_W_CLAMP;
	}
#endif

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

		m_ImageCapacity = bx::uint32_max(m_ImageCapacity + 4, handle.idx + 1);
		m_Images = (Image*)BX_REALLOC(m_Allocator, m_Images, sizeof(Image) * m_ImageCapacity);
		if (!m_Images) {
			return VG_INVALID_HANDLE;
		}

		// Reset all new textures...
		for (uint32_t i = oldCapacity; i < m_ImageCapacity; ++i) {
			m_Images[i].reset();
		}
	}

	BX_CHECK(handle.idx < m_ImageCapacity, "Allocated invalid image handle");
	Image* tex = &m_Images[handle.idx];
	BX_CHECK(!bgfx::isValid(tex->m_bgfxHandle), "Allocated texture is already in use");
	tex->reset();
	return handle;
}

FontHandle Context::loadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	if (m_NextFontID == VG_CONFIG_MAX_FONTS) {
		return VG_INVALID_HANDLE;
	}

	uint8_t* fontData = (uint8_t*)BX_ALLOC(m_Allocator, size);
	bx::memCopy(fontData, data, size);

	// Don't let FontStash free the font data because it uses the global allocator 
	// which might be different than m_Allocator.
	int fonsHandle = fonsAddFontMem(m_FontStashContext, name, fontData, size, 0);
	if (fonsHandle == FONS_INVALID) {
		BX_FREE(m_Allocator, fontData);
		return VG_INVALID_HANDLE;
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
	BX_CHECK(iw > 0 && ih > 0, "Invalid font atlas dimensions");

	// TODO: Convert only the dirty part of the texture (it's the only part that will be uploaded to the backend)
	uint32_t* rgbaData = (uint32_t*)BX_ALLOC(m_Allocator, sizeof(uint32_t) * iw * ih);
	convertA8_to_RGBA8(rgbaData, a8Data, (uint32_t)iw, (uint32_t)ih, 0x00FFFFFF);

	int x = dirty[0];
	int y = dirty[1];
	int w = dirty[2] - dirty[0];
	int h = dirty[3] - dirty[1];
	updateImage(fontImage, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, (const uint8_t*)rgbaData);

	BX_FREE(m_Allocator, rgbaData);
}

bool Context::updateImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data)
{
	if (!isValid(image)) {
		return false;
	}

	Image* tex = &m_Images[image.idx];
	BX_CHECK(bgfx::isValid(tex->m_bgfxHandle), "Invalid texture handle");

	const uint32_t bytesPerPixel = 4;
	const uint32_t pitch = tex->m_Width * bytesPerPixel;

	const bgfx::Memory* mem = bgfx::alloc(w * h * bytesPerPixel);
	bx::gather(mem->data, data + y * pitch + x * bytesPerPixel, w * bytesPerPixel, h, pitch);

	bgfx::updateTexture2D(tex->m_bgfxHandle, 0, 0, x, y, w, h, mem, UINT16_MAX);

	return true;
}

bool Context::deleteImage(ImageHandle img)
{
	if (!isValid(img)) {
		return false;
	}

	Image* tex = &m_Images[img.idx];
	BX_CHECK(bgfx::isValid(tex->m_bgfxHandle), "Invalid texture handle");
	bgfx::destroy(tex->m_bgfxHandle);
	tex->reset();
	
	m_ImageAlloc.free(img.idx);

	return true;
}

Vec2* Context::allocVertexBufferData_Vec2()
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	for (uint32_t i = 0; i < m_Vec2DataPoolCapacity; ++i) {
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)m_Vec2DataPool[i]) & 1) {
			// Remove the free flag
			m_Vec2DataPool[i] = (Vec2*)((uintptr_t)m_Vec2DataPool[i] & ~1);
			return m_Vec2DataPool[i];
		} else if (m_Vec2DataPool[i] == nullptr) {
			m_Vec2DataPool[i] = (Vec2*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(Vec2) * MAX_VB_VERTICES, 16);
			return m_Vec2DataPool[i];
		}
	}

	uint32_t oldCapacity = m_Vec2DataPoolCapacity;
	m_Vec2DataPoolCapacity += 8;
	m_Vec2DataPool = (Vec2**)BX_REALLOC(m_Allocator, m_Vec2DataPool, sizeof(Vec2*) * m_Vec2DataPoolCapacity);
	bx::memSet(&m_Vec2DataPool[oldCapacity], 0, sizeof(Vec2*) * (m_Vec2DataPoolCapacity - oldCapacity));

	m_Vec2DataPool[oldCapacity] = (Vec2*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(Vec2) * MAX_VB_VERTICES, 16);
	return m_Vec2DataPool[oldCapacity];
}

uint32_t* Context::allocVertexBufferData_Uint32()
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	for (uint32_t i = 0; i < m_Uint32DataPoolCapacity; ++i) {
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)m_Uint32DataPool[i]) & 1) {
			// Remove the free flag
			m_Uint32DataPool[i] = (uint32_t*)((uintptr_t)m_Uint32DataPool[i] & ~1);
			return m_Uint32DataPool[i];
		} else if (m_Uint32DataPool[i] == nullptr) {
			m_Uint32DataPool[i] = (uint32_t*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(uint32_t) * MAX_VB_VERTICES, 16);
			return m_Uint32DataPool[i];
		}
	}

	uint32_t oldCapacity = m_Uint32DataPoolCapacity;
	m_Uint32DataPoolCapacity += 8;
	m_Uint32DataPool = (uint32_t**)BX_REALLOC(m_Allocator, m_Uint32DataPool, sizeof(uint32_t*) * m_Uint32DataPoolCapacity);
	bx::memSet(&m_Uint32DataPool[oldCapacity], 0, sizeof(uint32_t*) * (m_Uint32DataPoolCapacity - oldCapacity));

	m_Uint32DataPool[oldCapacity] = (uint32_t*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(uint32_t) * MAX_VB_VERTICES, 16);
	return m_Uint32DataPool[oldCapacity];
}

void Context::releaseVertexBufferData_Vec2(Vec2* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	BX_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	for (uint32_t i = 0; i < m_Vec2DataPoolCapacity; ++i) {
		if (m_Vec2DataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			m_Vec2DataPool[i] = (Vec2*)((uintptr_t)m_Vec2DataPool[i] | 1);
			return;
		}
	}
}

void Context::releaseVertexBufferData_Uint32(uint32_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	BX_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	for (uint32_t i = 0; i < m_Uint32DataPoolCapacity; ++i) {
		if (m_Uint32DataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			m_Uint32DataPool[i] = (uint32_t*)((uintptr_t)m_Uint32DataPool[i] | 1);
			return;
		}
	}
}

inline void Context::tempGeomReset()
{
	m_TempGeomNumVertices = 0;
	m_TempGeomNumIndices = 0;
}

BX_FORCE_INLINE void Context::tempGeomExpandVB(uint32_t n)
{
	if (m_TempGeomNumVertices + n > m_TempGeomVertexCapacity) {
		tempGeomReallocVB(n);
	}
}

void Context::tempGeomReallocVB(uint32_t n)
{
	m_TempGeomVertexCapacity += n;
	m_TempGeomPos = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_TempGeomPos, sizeof(Vec2) * m_TempGeomVertexCapacity, 16);
	m_TempGeomColor = (uint32_t*)BX_ALIGNED_REALLOC(m_Allocator, m_TempGeomColor, sizeof(uint32_t) * m_TempGeomVertexCapacity, 16);
}

BX_FORCE_INLINE void Context::tempGeomExpandIB(uint32_t n)
{
	if (m_TempGeomNumIndices + n > m_TempGeomIndexCapacity) {
		tempGeomReallocIB(n);
	}
}

void Context::tempGeomReallocIB(uint32_t n)
{
	m_TempGeomIndexCapacity += n;
	m_TempGeomIndex = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, m_TempGeomIndex, sizeof(uint16_t) * m_TempGeomIndexCapacity, 16);
}

BX_FORCE_INLINE void Context::tempGeomAddPos(const Vec2* srcPos, uint32_t n)
{
	BX_CHECK(m_TempGeomNumVertices + n <= m_TempGeomVertexCapacity, "Not enough free space for temporary geometry");

	bx::memCopy(&m_TempGeomPos[m_TempGeomNumVertices], srcPos, sizeof(Vec2) * n);

	m_TempGeomNumVertices += n;
}

BX_FORCE_INLINE void Context::tempGeomAddPosColor(const Vec2* srcPos, const uint32_t* srcColor, uint32_t n)
{
	BX_CHECK(m_TempGeomNumVertices + n <= m_TempGeomVertexCapacity, "Not enough free space for temporary geometry");

	bx::memCopy(&m_TempGeomPos[m_TempGeomNumVertices], srcPos, sizeof(Vec2) * n);
	bx::memCopy(&m_TempGeomColor[m_TempGeomNumVertices], srcColor, sizeof(uint32_t) * n);

	m_TempGeomNumVertices += n;
}

BX_FORCE_INLINE void Context::tempGeomAddIndices(const uint16_t* src, uint32_t n)
{
	BX_CHECK(m_TempGeomNumIndices + n <= m_TempGeomIndexCapacity, "Not enough free space for temporary geometry");

	bx::memCopy(&m_TempGeomIndex[m_TempGeomNumIndices], src, sizeof(uint16_t) * n);

	m_TempGeomNumIndices += n;
}

String* Context::createString(const Font& font, const char* text, const char* end)
{
	const uint32_t textSize = (uint32_t)(end ? end - text : bx::strLen(text));
	const uint32_t bufSize = sizeof(String) + textSize + 1 + sizeof(FONSstring);
	uint8_t* buf = (uint8_t*)BX_ALLOC(m_Allocator, bufSize);

	String* str = (String*)buf;
	str->m_RendererData = buf + sizeof(String);
	str->m_Text = (char*)(buf + sizeof(String) + sizeof(FONSstring));
	str->m_Font = font;

	bx::memCopy(str->m_Text, text, textSize);
	str->m_Text[textSize] = 0;

	FONSstring* fs = (FONSstring*)str->m_RendererData;
	fonsInitString(fs);
	fonsSetFont(m_FontStashContext, font.m_Handle.idx);
	fonsResetString(m_FontStashContext, fs, str->m_Text, nullptr);

	return str;
}

void Context::destroyString(String* str)
{
	fonsDestroyString((FONSstring*)str->m_RendererData);
	BX_FREE(m_Allocator, str);
}

void Context::text(String* str, uint32_t alignment, Color color, float x, float y)
{
	const State* state = getState();
	const float scale = state->m_FontScale * m_DevicePixelRatio;
	const float scaledFontSize = str->m_Font.m_Size * scale;
	if (scaledFontSize < 1.0f) { // TODO: Make threshold configurable.
		return;
	}

	fonsSetSize(m_FontStashContext, scaledFontSize);
	fonsSetFont(m_FontStashContext, str->m_Font.m_Handle.idx);

	FONSstring* fs = (FONSstring*)str->m_RendererData;
//	fonsResetString(m_FontStashContext, fs, str->m_Text, nullptr);

	uint32_t numBakedChars = fonsBakeString(m_FontStashContext, fs);
	if (numBakedChars == ~0u) {
		// Atlas full? Retry
		if (!allocTextAtlas()) {
			return;
		}

		numBakedChars = fonsBakeString(m_FontStashContext, fs);
	}

	if (numBakedChars == ~0u || numBakedChars == 0) {
		return;
	}

	if (m_TextQuadCapacity < numBakedChars) {
		m_TextQuadCapacity = numBakedChars;
		m_TextQuads = (FONSquad*)BX_ALIGNED_REALLOC(m_Allocator, m_TextQuads, sizeof(FONSquad) * m_TextQuadCapacity, 16);
		m_TextVertices = (Vec2*)BX_ALIGNED_REALLOC(m_Allocator, m_TextVertices, sizeof(Vec2) * m_TextQuadCapacity * 4, 16);
	}

	bx::memCopy(m_TextQuads, fs->m_Quads, sizeof(FONSquad) * numBakedChars);

	float dx = 0.0f, dy = 0.0f;
	fonsAlignString(m_FontStashContext, fs, alignment, &dx, &dy);

	pushState();
	transformTranslate(x + dx / scale, y + dy / scale);
	renderTextQuads(numBakedChars, color);
	popState();
}

// Concave polygon decomposition
// http://www.flipcode.com/archives/Efficient_Polygon_Triangulation.shtml
static float Area(const Vec2* points, uint32_t numPoints)
{
	float A = 0.0f;
	for (uint32_t p = numPoints - 1, q = 0; q < numPoints; p = q++) {
		A += points[p].x * points[q].y - points[q].x * points[p].y;
	}

	return A * 0.5f;
}

inline bool InsideTriangle(const Vec2& A, const Vec2& B, const Vec2& C, const Vec2& P)
{
	const Vec2 a = C - B;
	const Vec2 bp = P - B;
	const float aCROSSbp = a.x * bp.y - a.y * bp.x;
	if (aCROSSbp < 0.0f) {
		return false;
	}

	const Vec2 c = B - A;
	const Vec2 ap = P - A;
	const float cCROSSap = c.x * ap.y - c.y * ap.x;
	if (cCROSSap < 0.0f) {
		return false;
	}

	const Vec2 b = A - C;
	const Vec2 cp = P - C;
	const float bCROSScp = b.x * cp.y - b.y * cp.x;
	if (bCROSScp < 0.0f) {
		return false;
	}

	return true;
};

static bool Snip(const Vec2* points, uint32_t numPoints, int u, int v, int w, int n, const int* V)
{
	BX_UNUSED(numPoints);

	const Vec2& A = points[V[u]];
	const Vec2& B = points[V[v]];
	const Vec2& C = points[V[w]];

	const Vec2 AB = B - A;
	const Vec2 AC = C - A;
	const float AB_cross_AC = AB.x * AC.y - AB.y * AC.x;
	if (AB_cross_AC < 1e-6f) {
		return false;
	}

	for (int p = 0; p < n; p++) {
		if ((p == u) || (p == v) || (p == w)) {
			continue;
		}

		const Vec2& P = points[V[p]];
		if (InsideTriangle(A, B, C, P)) {
			return false;
		}
	}

	return true;
}

void Context::decomposeAndFillConcavePolygon(const Vec2* points, uint32_t numPoints, Color col, bool aa)
{
	// TODO: Correct AA of concave polygons requires keeping track of original polygon edges
	// and generating AA fringes only for those edges.
	// NOTE: On second thought, there's no need to keep track of any edge. Every edge from the original
	// polygon is between i and i+1 vertices. So I might be able to generate AA fringes either way (they
	// are like half strokes on the outside of the polygon).
	BX_UNUSED(aa);
	BX_CHECK(numPoints >= 3, "Invalid number of points in polygon");

	const State* state = getState();
	const uint32_t c = ColorRGBA::setAlpha(col, (uint8_t)(state->m_GlobalAlpha * ColorRGBA::getAlpha(col)));
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	// allocate and initialize list of Vertices in polygon
	// TODO: Avoid allocation
	int* V = (int*)BX_ALLOC(m_Allocator, sizeof(int) * numPoints);

	tempGeomReset();

	// we want a counter-clockwise polygon in V
	if (Area(points, numPoints) > 0.0f) {
		for (uint32_t v = 0; v < numPoints; ++v) {
			V[v] = v;
		}
	} else {
		const uint32_t last = numPoints - 1;
		for (uint32_t v = 0; v < numPoints; ++v) {
			V[v] = last - v;
		}
	}

	int n = (int)numPoints;
	int nv = n;

	// remove nv - 2 Vertices, creating 1 triangle every time
	int count = 2 * nv;   // error detection
	for (int v = nv - 1; nv > 2;) {
		// if we loop, it is probably a non-simple polygon
		if ((count--) <= 0) {
			BX_WARN(false, "Not a simple polygon. Falling back to convex rendering.");
			BX_FREE(m_Allocator, V);
			renderConvexPolygonNoAA(points, numPoints, col);
			return;
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

		if (Snip(points, numPoints, u, v, w, nv, V)) {
			// true names of the vertices
			uint16_t id[3] = {
				(uint16_t)V[u],
				(uint16_t)V[v],
				(uint16_t)V[w]
			};

			// output Triangle
			tempGeomExpandIB(3);
			tempGeomAddIndices(&id[0], 3);

			// remove v from remaining polygon
			// TODO: Check if this can be avoided (e.g.) by moving the V ptr (probably won't work but worth the check)
			--nv;
			for (int s = v; s < nv; ++s) {
				V[s] = V[s + 1];
			}

			// reses error detection counter
			count = 2 * nv;
		}
	}
	
	BX_FREE(m_Allocator, V);

	// Create draw command
	const Vec2 uv = getWhitePixelUV();
	const uint32_t numDrawVertices = numPoints;
	const uint32_t numDrawIndices = m_TempGeomNumIndices;

	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numDrawVertices, numDrawIndices, m_FontImages[0]);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	Vec2* dstPos = &vb->m_Pos[vbOffset];
	bx::memCopy(dstPos, points, sizeof(Vec2) * numDrawVertices);

	Vec2* dstUV = &vb->m_UV[vbOffset];
	memset64(dstUV, numDrawVertices, &uv.x);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numDrawVertices, &c);

	const uint32_t startVertexID = cmd->m_NumVertices;
	uint16_t* dstIndex = &m_IndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(m_TempGeomIndex, m_TempGeomNumIndices, dstIndex, (uint16_t)startVertexID);

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

//////////////////////////////////////////////////////////////////////////
// SIMD functions
//
void batchTransformTextQuads(const FONSquad* __restrict quads, uint32_t n, const float* __restrict mtx, Vec2* __restrict transformedVertices)
{
#if VG_CONFIG_ENABLE_SIMD
	const bx::simd128_t mtx0 = bx::simd_splat(mtx[0]);
	const bx::simd128_t mtx1 = bx::simd_splat(mtx[1]);
	const bx::simd128_t mtx2 = bx::simd_splat(mtx[2]);
	const bx::simd128_t mtx3 = bx::simd_splat(mtx[3]);
	const bx::simd128_t mtx4 = bx::simd_splat(mtx[4]);
	const bx::simd128_t mtx5 = bx::simd_splat(mtx[5]);

	const uint32_t iter = n >> 1; // 2 quads per iteration
	for (uint32_t i = 0; i < iter; ++i) {
		bx::simd128_t q0 = bx::simd_ld(quads);     // (x0, y0, x1, y1)
		bx::simd128_t q1 = bx::simd_ld(quads + 1); // (x2, y2, x3, y3)

		bx::simd128_t x0123 = bx::simd_shuf_xyAB(bx::simd_swiz_xzxz(q0), bx::simd_swiz_xzxz(q1)); // (x0, x1, x2, x3)
		bx::simd128_t y0123 = bx::simd_shuf_xyAB(bx::simd_swiz_ywyw(q0), bx::simd_swiz_ywyw(q1)); // (y0, y1, y2, y3)
		bx::simd128_t x0123_m0 = bx::simd_mul(x0123, mtx0); // (x0, x1, x2, x3) * mtx[0]
		bx::simd128_t x0123_m1 = bx::simd_mul(x0123, mtx1); // (x0, x1, x2, x3) * mtx[1]
		bx::simd128_t y0123_m2 = bx::simd_mul(y0123, mtx2); // (y0, y1, y2, y3) * mtx[2]
		bx::simd128_t y0123_m3 = bx::simd_mul(y0123, mtx3); // (y0, y1, y2, y3) * mtx[3]

		// v0.x = x0_m0 + y0_m2 + m4
		// v1.x = x1_m0 + y0_m2 + m4
		// v2.x = x1_m0 + y1_m2 + m4
		// v3.x = x0_m0 + y1_m2 + m4
		// v0.y = x0_m1 + y0_m3 + m5
		// v1.y = x1_m1 + y0_m3 + m5
		// v2.y = x1_m1 + y1_m3 + m5
		// v3.y = x0_m1 + y1_m3 + m5
		bx::simd128_t x0110_m0 = bx::simd_swiz_xyyx(x0123_m0);
		bx::simd128_t x0110_m1 = bx::simd_swiz_xyyx(x0123_m1);
		bx::simd128_t y0011_m2 = bx::simd_swiz_xxyy(y0123_m2);
		bx::simd128_t y0011_m3 = bx::simd_swiz_xxyy(y0123_m3);

		bx::simd128_t v0123_x = bx::simd_add(x0110_m0, bx::simd_add(y0011_m2, mtx4));
		bx::simd128_t v0123_y = bx::simd_add(x0110_m1, bx::simd_add(y0011_m3, mtx5));

		bx::simd128_t v01 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(v0123_x, v0123_y));
		bx::simd128_t v23 = bx::simd_swiz_xzyw(bx::simd_shuf_zwCD(v0123_x, v0123_y));

		bx::simd_st(transformedVertices, v01);
		bx::simd_st(transformedVertices + 2, v23);

		// v4.x = x2_m0 + y2_m2 + m4
		// v5.x = x3_m0 + y2_m2 + m4
		// v6.x = x3_m0 + y3_m2 + m4
		// v7.x = x2_m0 + y3_m2 + m4
		// v4.y = x2_m1 + y2_m3 + m5
		// v5.y = x3_m1 + y2_m3 + m5
		// v6.y = x3_m1 + y3_m3 + m5
		// v7.y = x2_m1 + y3_m3 + m5
		bx::simd128_t x2332_m0 = bx::simd_swiz_zwwz(x0123_m0);
		bx::simd128_t x2332_m1 = bx::simd_swiz_zwwz(x0123_m1);
		bx::simd128_t y2233_m2 = bx::simd_swiz_zzww(y0123_m2);
		bx::simd128_t y2233_m3 = bx::simd_swiz_zzww(y0123_m3);

		bx::simd128_t v4567_x = bx::simd_add(x2332_m0, bx::simd_add(y2233_m2, mtx4));
		bx::simd128_t v4567_y = bx::simd_add(x2332_m1, bx::simd_add(y2233_m3, mtx5));

		bx::simd128_t v45 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(v4567_x, v4567_y));
		bx::simd128_t v67 = bx::simd_swiz_xzyw(bx::simd_shuf_zwCD(v4567_x, v4567_y));

		bx::simd_st(transformedVertices + 4, v45);
		bx::simd_st(transformedVertices + 6, v67);

		quads += 2;
		transformedVertices += 8;
	}

	const uint32_t rem = n & 1;
	if (rem) {
		bx::simd128_t q0 = bx::simd_ld(quads);

		bx::simd128_t x0101 = bx::simd_swiz_xzxz(q0); // (x0, x1, x0, x1)
		bx::simd128_t y0101 = bx::simd_swiz_ywyw(q0); // (y0, y1, y0, y1)
		bx::simd128_t x0101_m0 = bx::simd_mul(x0101, mtx0); // (x0, x1, x0, x1) * mtx[0]
		bx::simd128_t x0101_m1 = bx::simd_mul(x0101, mtx1); // (x0, x1, x0, x1) * mtx[1]
		bx::simd128_t y0101_m2 = bx::simd_mul(y0101, mtx2); // (y0, y1, y0, y1) * mtx[2]
		bx::simd128_t y0101_m3 = bx::simd_mul(y0101, mtx3); // (y0, y1, y0, y1) * mtx[3]

		// v0.x = x0_m0 + y0_m2 + m4
		// v1.x = x1_m0 + y0_m2 + m4
		// v2.x = x1_m0 + y1_m2 + m4
		// v3.x = x0_m0 + y1_m2 + m4
		// v0.y = x0_m1 + y0_m3 + m5
		// v1.y = x1_m1 + y0_m3 + m5
		// v2.y = x1_m1 + y1_m3 + m5
		// v3.y = x0_m1 + y1_m3 + m5
		bx::simd128_t x0110_m0 = bx::simd_swiz_xyyx(x0101_m0);
		bx::simd128_t x0110_m1 = bx::simd_swiz_xyyx(x0101_m1);
		bx::simd128_t y0011_m2 = bx::simd_swiz_xxyy(y0101_m2);
		bx::simd128_t y0011_m3 = bx::simd_swiz_xxyy(y0101_m3);

		bx::simd128_t v0123_x = bx::simd_add(x0110_m0, bx::simd_add(y0011_m2, mtx4));
		bx::simd128_t v0123_y = bx::simd_add(x0110_m1, bx::simd_add(y0011_m3, mtx5));

		bx::simd128_t v01 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(v0123_x, v0123_y));
		bx::simd128_t v23 = bx::simd_swiz_xzyw(bx::simd_shuf_zwCD(v0123_x, v0123_y));

		bx::simd_st(transformedVertices, v01);
		bx::simd_st(transformedVertices + 2, v23);
	}
#else
	for (uint32_t i = 0; i < n; ++i) {
		const FONSquad* q = &quads[i];
		const uint32_t s = i << 2;
		transformedVertices[s + 0] = transformPos2D(q->x0, q->y0, mtx);
		transformedVertices[s + 1] = transformPos2D(q->x1, q->y0, mtx);
		transformedVertices[s + 2] = transformPos2D(q->x1, q->y1, mtx);
		transformedVertices[s + 3] = transformPos2D(q->x0, q->y1, mtx);
	}
#endif
}

void batchTransformPositions(const Vec2* __restrict v, uint32_t n, Vec2* __restrict p, const float* __restrict mtx)
{
#if !VG_CONFIG_ENABLE_SIMD
	for (uint32_t i = 0; i < n; ++i) {
		p[i] = transformPos2D(v[i].x, v[i].y, mtx);
	}
#else
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
#endif
}

void batchTransformPositions_Unaligned(const Vec2* __restrict v, uint32_t n, Vec2* __restrict p, const float* __restrict mtx)
{
#if !VG_CONFIG_ENABLE_SIMD
	for (uint32_t i = 0; i < n; ++i) {
		p[i] = transformPos2D(v[i].x, v[i].y, mtx);
	}
#else
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
		bx::simd128_t src0123 = _mm_loadu_ps(src);
		bx::simd128_t src4567 = _mm_loadu_ps(src + 4);

		bx::simd128_t src0246 = bx::simd_shuf_xyAB(bx::simd_swiz_xzxz(src0123), bx::simd_swiz_xzxz(src4567));
		bx::simd128_t src1357 = bx::simd_shuf_xyAB(bx::simd_swiz_ywyw(src0123), bx::simd_swiz_ywyw(src4567));

		bx::simd128_t dst0246 = bx::simd_add(bx::simd_add(bx::simd_mul(src0246, mtx0), bx::simd_mul(src1357, mtx2)), mtx4);
		bx::simd128_t dst1357 = bx::simd_add(bx::simd_add(bx::simd_mul(src0246, mtx1), bx::simd_mul(src1357, mtx3)), mtx5);

		bx::simd128_t dst0123 = bx::simd_swiz_xzyw(bx::simd_shuf_xyAB(dst0246, dst1357));
		bx::simd128_t dst4567 = bx::simd_swiz_xzyw(bx::simd_shuf_zwCD(dst0246, dst1357));

		_mm_storeu_ps(dst, dst0123);
		_mm_storeu_ps(dst + 4, dst4567);

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
#endif
}

// NOTE: Assumes src is 16-byte aligned. Don't care about dst (unaligned stores)
void batchTransformDrawIndices(const uint16_t* __restrict src, uint32_t n, uint16_t* __restrict dst, uint16_t delta)
{
#if !VG_CONFIG_ENABLE_SIMD
	for (uint32_t i = 0; i < n; ++i) {
		*dst++ = *src + delta;
		src++;
	}
#else
	const __m128i xmm_delta = _mm_set1_epi16(delta);

	const uint32_t iter32 = n >> 5;
	for (uint32_t i = 0; i < iter32; ++i) {
		__m128i s0 = _mm_load_si128((const __m128i*)src);
		__m128i s1 = _mm_load_si128((const __m128i*)(src + 8));
		__m128i s2 = _mm_load_si128((const __m128i*)(src + 16));
		__m128i s3 = _mm_load_si128((const __m128i*)(src + 24));

		__m128i d0 = _mm_add_epi16(s0, xmm_delta);
		__m128i d1 = _mm_add_epi16(s1, xmm_delta);
		__m128i d2 = _mm_add_epi16(s2, xmm_delta);
		__m128i d3 = _mm_add_epi16(s3, xmm_delta);

		// NOTE: Proper alignment of dst buffer isn't guaranteed because it's part of the global IndexBuffer.
		_mm_storeu_si128((__m128i*)dst, d0);
		_mm_storeu_si128((__m128i*)(dst + 8), d1);
		_mm_storeu_si128((__m128i*)(dst + 16), d2);
		_mm_storeu_si128((__m128i*)(dst + 24), d3);

		src += 32;
		dst += 32;
	}

	uint32_t rem = n & 31;
	if (rem >= 16) {
		__m128i s0 = _mm_load_si128((const __m128i*)src);
		__m128i s1 = _mm_load_si128((const __m128i*)(src + 8));
		__m128i d0 = _mm_add_epi16(s0, xmm_delta);
		__m128i d1 = _mm_add_epi16(s1, xmm_delta);
		_mm_storeu_si128((__m128i*)dst, d0);
		_mm_storeu_si128((__m128i*)(dst + 8), d1);

		src += 16;
		dst += 16;
		rem -= 16;
	}

	if (rem >= 8) {
		__m128i s0 = _mm_load_si128((const __m128i*)src);
		__m128i d0 = _mm_add_epi16(s0, xmm_delta);
		_mm_storeu_si128((__m128i*)dst, d0);

		src += 8;
		dst += 8;
		rem -= 8;
	}

	switch (rem) {
	case 7:
		*dst++ = *src++ + delta;
	case 6:
		*dst++ = *src++ + delta;
	case 5:
		*dst++ = *src++ + delta;
	case 4:
		*dst++ = *src++ + delta;
	case 3:
		*dst++ = *src++ + delta;
	case 2:
		*dst++ = *src++ + delta;
	case 1:
		*dst++ = *src++ + delta;
	}
#endif
}
}
