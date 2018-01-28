// TODO:
// 4) Layers
// 5) Check polygon winding and force CCW order because otherwise AA fringes are generated inside the polygon!
#include "vg.h"
#include "bgfxvg_renderer.h"
#include "shape.h"
#include "path.h"
#include "stroker.h"

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
#include "shaders/vs_solid_color.bin.h"
#include "shaders/fs_solid_color.bin.h"
#include "shaders/vs_gradient.bin.h"
#include "shaders/fs_gradient.bin.h"
#include "shaders/vs_stencil.bin.h"
#include "shaders/fs_stencil.bin.h"

BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4127) // conditional expression is constant (e.g. BezierTo)
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4706) // assignment within conditional expression
BX_PRAGMA_DIAGNOSTIC_IGNORED_CLANG_GCC("-Wshadow");

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
	BGFX_EMBEDDED_SHADER(vs_stencil),
	BGFX_EMBEDDED_SHADER(fs_stencil),

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
	float* m_Pos;
#if VG_CONFIG_UV_INT16
	int16_t* m_UV;
#else
	float* m_UV;
#endif
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

struct ClipState
{
	ClipRule::Enum m_Rule;
	uint32_t m_FirstCmdID;
	uint32_t m_NumCmds;
};

struct DrawCommand
{
	enum Type : uint32_t
	{
		Type_TexturedVertexColor = 0,
		Type_ColorGradient = 1,
		Type_Clip = 2,
		NumTypes
	};

	Type m_Type;
	ClipState m_ClipState;
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
	float* m_Pos;
#if VG_CONFIG_UV_INT16
	int16_t* m_UV;
#else
	float* m_UV;
#endif
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
	float m_Pos[2];
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

	float** m_Vec2DataPool;
	uint32_t** m_Uint32DataPool;
#if VG_CONFIG_UV_INT16
	int16_t** m_UVDataPool;
#endif
	uint32_t m_Vec2DataPoolCapacity;
	uint32_t m_Uint32DataPoolCapacity;
#if VG_CONFIG_UV_INT16
	uint32_t m_UVDataPoolCapacity;
#endif
#if BX_CONFIG_SUPPORTS_THREADING
	bx::Mutex m_DataPoolMutex;
#endif

	DrawCommand* m_DrawCommands;
	uint32_t m_NumDrawCommands;
	uint32_t m_DrawCommandCapacity;

	ClipState m_ClipState;
	DrawCommand* m_ClipCommands;
	uint32_t m_NumClipCommands;
	uint32_t m_ClipCommandCapacity;
	bool m_RecordClipCommands;

	Image* m_Images;
	uint32_t m_ImageCapacity;
	bx::HandleAllocT<MAX_TEXTURES> m_ImageAlloc;

	Path* m_Path;
	Stroker* m_Stroker;
	float* m_TransformedPathVertices;
	uint32_t m_NumTransformedPathVertices;
	bool m_PathVerticesTransformed;

	State m_StateStack[MAX_STATE_STACK_SIZE];
	uint32_t m_CurStateID;

	IndexBuffer* m_IndexBuffers;
	IndexBuffer* m_ActiveIndexBuffer;
	uint32_t m_NumIndexBuffers;

	FONSquad* m_TextQuads;
	float* m_TextVertices;
	uint32_t m_TextQuadCapacity;

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
	bool m_ForceNewClipCommand;

	Context(bx::AllocatorI* allocator, uint8_t viewID);
	~Context();

	// Helpers...
	inline State* getState()               { return &m_StateStack[m_CurStateID]; }
	inline VertexBuffer* getVertexBuffer() { return &m_VertexBuffers[m_NumVertexBuffers - 1]; }
#if VG_CONFIG_UV_INT16
	inline void getWhitePixelUV(int16_t* uv)
	{
		int w, h;
		getImageSize(m_FontImages[0], &w, &h);
		uv[0] = INT16_MAX / (int16_t)w;
		uv[1] = INT16_MAX / (int16_t)h;
	}
#else
	inline void getWhitePixelUV(float* uv)
	{
		int w, h;
		getImageSize(m_FontImages[0], &w, &h);
		uv[0] = 0.5f / (float)w;
		uv[1] = 0.5f / (float)h;
	}
#endif

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
	void roundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
	void circle(float cx, float cy, float r);
	void polyline(const float* coords, uint32_t numPoints);
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

	// Clipping
	void beginClip(ClipRule::Enum rule);
	void endClip();
	void resetClip();

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
	float* allocVertexBufferData_Vec2();
	uint32_t* allocVertexBufferData_Uint32();
	void releaseVertexBufferData_Vec2(float* data);
	void releaseVertexBufferData_Uint32(uint32_t* data);
#if VG_CONFIG_UV_INT16
	int16_t* allocVertexBufferData_UV();
	void releaseVertexBufferData_UV(int16_t* data);
#endif

	// Index buffers
	IndexBuffer* allocIndexBuffer();
	void releaseIndexBuffer(uint16_t* data);

	// Draw commands
	DrawCommand* allocDrawCommand_TexturedVertexColor(uint32_t numVertices, uint32_t numIndices, ImageHandle img);
	DrawCommand* allocDrawCommand_ColorGradient(uint32_t numVertices, uint32_t numIndices, GradientHandle gradient);
	DrawCommand* allocDrawCommand_Clip(uint32_t numVertices, uint32_t numIndices);
	void createDrawCommand_VertexColor(const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices);
	void createDrawCommand_Textured(const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, ImageHandle imgHandle, const float* uvMatrix, const uint16_t* indices, uint32_t numIndices);
	void createDrawCommand_Gradient(const float* vtx, uint32_t numVertices, GradientHandle gradientHandle, const uint16_t* indices, uint32_t numIndices);
	void createDrawCommand_Clip(const float* vtx, uint32_t numVertices, const uint16_t* indices, uint32_t numIndices);

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
	void renderTextQuads(uint32_t numQuads, Color color);

	// Caching
	void cachedShapeReset(CachedShape* shape);
	void cachedShapeAddDrawCommand(CachedShape* shape, const DrawCommand* cmd);
	void cachedShapeAddTextCommand(CachedShape* shape, const uint8_t* cmdData);
	void cachedShapeAddTextCommand(CachedShape* shape, String* str, uint32_t alignment, Color col, float x, float y);
	void cachedShapeRender(const CachedShape* shape, GetStringByIDFunc* stringCallback, void* userData);

	// Strings
	String* createString(const Font& font, const char* text, const char* end);
	void destroyString(String* str);
	void text(String* str, uint32_t alignment, Color color, float x, float y);
};

static void releaseVertexBufferDataCallback_Vec2(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseVertexBufferData_Vec2((float*)ptr);
}

static void releaseVertexBufferDataCallback_Uint32(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseVertexBufferData_Uint32((uint32_t*)ptr);
}

#if VG_CONFIG_UV_INT16
static void releaseVertexBufferDataCallback_UV(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseVertexBufferData_UV((int16_t*)ptr);
}
#endif

static void releaseIndexBufferCallback(void* ptr, void* userData)
{
	Context* ctx = (Context*)userData;
	ctx->releaseIndexBuffer((uint16_t*)ptr);
}

inline void transformPos2D(float x, float y, const float* mtx, float* res)
{
	res[0] = mtx[0] * x + mtx[2] * y + mtx[4];
	res[1] = mtx[1] * x + mtx[3] * y + mtx[5];
}

inline void transformVec2D(float x, float y, const float* mtx, float* res)
{
	res[0] = mtx[0] * x + mtx[2] * y;
	res[1] = mtx[1] * x + mtx[3] * y;
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
#if 0
	float sx = bx::sqrt(t[0] * t[0] + t[2] * t[2]);
	float sy = bx::sqrt(t[1] * t[1] + t[3] * t[3]);
#else
	float sx = sqrtf(t[0] * t[0] + t[2] * t[2]);
	float sy = sqrtf(t[1] * t[1] + t[3] * t[3]);
#endif
	return (sx + sy) * 0.5f;
}

inline float quantize(float a, float d)
{
	return ((int)(a / d + 0.5f)) * d;
}

inline float getFontScale(const float* xform)
{
	return bx::min<float>(quantize(getAverageScale(xform), 0.01f), 4.0f);
}

void batchTransformPositions(const float* __restrict v, uint32_t n, float* __restrict p, const float* __restrict mtx);
void batchTransformPositions_Unaligned(const float* __restrict v, uint32_t n, float* __restrict p, const float* __restrict mtx);
void batchTransformDrawIndices(const uint16_t* __restrict src, uint32_t n, uint16_t* __restrict dst, uint16_t delta);
void batchTransformTextQuads(const FONSquad* __restrict quads, uint32_t n, const float* __restrict mtx, float* __restrict transformedVertices);
#if VG_CONFIG_UV_INT16
void generateUVs(const float* __restrict v, uint32_t n, int16_t* __restrict uv, const float* __restrict mtx);
#endif

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

void BGFXVGRenderer::RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	m_Context->roundedRectVarying(x, y, w, h, rtl, rbl, rbr, rtr);
}

void BGFXVGRenderer::Circle(float cx, float cy, float r)
{
	m_Context->circle(cx, cy, r);
}

void BGFXVGRenderer::Polyline(const float* coords, uint32_t numPoints)
{
	m_Context->polyline(coords, numPoints);
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

void BGFXVGRenderer::BeginClip(ClipRule::Enum rule)
{
	m_Context->beginClip(rule);
}

void BGFXVGRenderer::EndClip()
{
	m_Context->endClip();
}

void BGFXVGRenderer::ResetClip()
{
	m_Context->resetClip();
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
Context::Context(bx::AllocatorI* allocator, uint8_t viewID) 
	: m_Allocator(allocator)
	, m_VertexBuffers(nullptr)
	, m_NumVertexBuffers(0)
	, m_VertexBufferCapacity(0)
	, m_Vec2DataPool(nullptr)
	, m_Uint32DataPool(nullptr)
	, m_Vec2DataPoolCapacity(0)
	, m_Uint32DataPoolCapacity(0)
#if VG_CONFIG_UV_INT16
	, m_UVDataPool(nullptr)
	, m_UVDataPoolCapacity(0)
#endif
	, m_DrawCommands(nullptr)
	, m_NumDrawCommands(0)
	, m_DrawCommandCapacity(0)
	, m_ClipCommands(nullptr)
	, m_NumClipCommands(0)
	, m_ClipCommandCapacity(0)
	, m_Images(nullptr)
	, m_ImageCapacity(0)
	, m_Path(nullptr)
	, m_Stroker(nullptr)
	, m_TransformedPathVertices(nullptr)
	, m_NumTransformedPathVertices(0)
	, m_PathVerticesTransformed(false)
	, m_CurStateID(0)
	, m_IndexBuffers(nullptr)
	, m_NumIndexBuffers(0)
	, m_ActiveIndexBuffer(nullptr)
	, m_TextQuads(nullptr)
	, m_TextVertices(nullptr)
	, m_TextQuadCapacity(0)
	, m_FontStashContext(nullptr)
	, m_FontImageID(0)
	, m_WinWidth(0)
	, m_WinHeight(0)
	, m_DevicePixelRatio(1.0f)
	, m_TesselationTolerance(1.0f)
	, m_FringeWidth(1.0f)
	, m_ViewID(viewID)
	, m_ForceNewDrawCommand(false)
	, m_ForceNewClipCommand(false)
	, m_NextFontID(0)
	, m_NextGradientID(0)
	, m_NextImagePatternID(0)
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
			m_ProgramHandle[i] = BGFX_INVALID_HANDLE;
		}
	}

	bgfx::destroy(m_TexUniform);
	bgfx::destroy(m_PaintMatUniform);
	bgfx::destroy(m_ExtentRadiusFeatherUniform);
	bgfx::destroy(m_InnerColorUniform);
	bgfx::destroy(m_OuterColorUniform);

	for (uint32_t i = 0; i < m_VertexBufferCapacity; ++i) {
		VertexBuffer* vb = &m_VertexBuffers[i];
		if (bgfx::isValid(vb->m_PosBufferHandle)) {
			bgfx::destroy(vb->m_PosBufferHandle);
			vb->m_PosBufferHandle = BGFX_INVALID_HANDLE;
		}
		if (bgfx::isValid(vb->m_UVBufferHandle)) {
			bgfx::destroy(vb->m_UVBufferHandle);
			vb->m_UVBufferHandle = BGFX_INVALID_HANDLE;
		}
		if (bgfx::isValid(vb->m_ColorBufferHandle)) {
			bgfx::destroy(vb->m_ColorBufferHandle);
			vb->m_ColorBufferHandle = BGFX_INVALID_HANDLE;
		}
	}
	BX_FREE(m_Allocator, m_VertexBuffers);
	m_VertexBuffers = nullptr;
	m_VertexBufferCapacity = 0;
	m_NumVertexBuffers = 0;

	for (uint32_t i = 0; i < m_NumIndexBuffers; ++i) {
		IndexBuffer* ib = &m_IndexBuffers[i];
		if (bgfx::isValid(ib->m_bgfxHandle)) {
			bgfx::destroy(ib->m_bgfxHandle);
			ib->m_bgfxHandle = BGFX_INVALID_HANDLE;
		}

		BX_ALIGNED_FREE(m_Allocator, ib->m_Indices, 16);
		ib->m_Indices = nullptr;
		ib->m_Capacity = 0;
		ib->m_Count = 0;
	}
	BX_FREE(m_Allocator, m_IndexBuffers);
	m_IndexBuffers = nullptr;
	m_ActiveIndexBuffer = nullptr;

	for (uint32_t i = 0; i < m_Vec2DataPoolCapacity; ++i) {
		float* buffer = m_Vec2DataPool[i];
		if (!buffer) {
			continue;
		}

		if ((uintptr_t)buffer & 1) {
			buffer = (float*)((uintptr_t)buffer & ~1);
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
	m_DrawCommands = nullptr;

	// Manually delete font data
	for (int i = 0; i < VG_CONFIG_MAX_FONTS; ++i) {
		if (m_FontData[i]) {
			BX_FREE(m_Allocator, m_FontData[i]);
			m_FontData[i] = nullptr;
		}
	}

	fonsDeleteInternal(m_FontStashContext);

	for (uint32_t i = 0; i < MAX_FONT_IMAGES; ++i) {
		deleteImage(m_FontImages[i]);		
	}

	BX_DELETE(m_Allocator, m_Path);
	m_Path = nullptr;

	BX_DELETE(m_Allocator, m_Stroker);
	m_Stroker = nullptr;

	BX_FREE(m_Allocator, m_Images);
	m_Images = nullptr;

	BX_ALIGNED_FREE(m_Allocator, m_TextQuads, 16);
	m_TextQuads = nullptr;

	BX_ALIGNED_FREE(m_Allocator, m_TextVertices, 16);
	m_TextVertices = nullptr;

	BX_ALIGNED_FREE(m_Allocator, m_TransformedPathVertices, 16);
	m_TransformedPathVertices = nullptr;
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
	m_Stroker = BX_NEW(m_Allocator, Stroker)(m_Allocator);
	
	m_PosVertexDecl.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float).end();
#if VG_CONFIG_UV_INT16
	m_UVVertexDecl.begin().add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Int16, true).end();
#else
	m_UVVertexDecl.begin().add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
#endif
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
	m_ProgramHandle[DrawCommand::Type_Clip] =
		bgfx::createProgram(
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "vs_stencil"),
		bgfx::createEmbeddedShader(s_EmbeddedShaders, bgfxRendererType, "fs_stencil"),
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
	VG_CHECK(windowWidth > 0 && windowWidth < 65536, "Invalid window width");
	VG_CHECK(windowHeight > 0 && windowHeight < 65536, "Invalid window height");
	m_WinWidth = (uint16_t)windowWidth;
	m_WinHeight = (uint16_t)windowHeight;
	m_DevicePixelRatio = devicePixelRatio;
	m_TesselationTolerance = 0.25f / devicePixelRatio;
	m_FringeWidth = 1.0f / devicePixelRatio;

	VG_CHECK(m_CurStateID == 0, "State stack hasn't been properly reset in the previous frame");
	resetScissor();
	transformIdentity();

	m_NumVertexBuffers = 0;
	allocVertexBuffer();

	m_ActiveIndexBuffer = allocIndexBuffer();
	VG_CHECK(m_ActiveIndexBuffer->m_Count == 0, "Not empty index buffer");

	m_NumDrawCommands = 0;
	m_ForceNewDrawCommand = true;

	m_NumClipCommands = 0;
	m_ForceNewClipCommand = true;
	m_ClipState.m_FirstCmdID = ~0u;
	m_ClipState.m_NumCmds = 0;
	m_ClipState.m_Rule = ClipRule::In;

	m_NextGradientID = 0;
	m_NextImagePatternID = 0;
}

void Context::endFrame()
{
	VG_CHECK(m_CurStateID == 0, "PushState()/PopState() mismatch");
	if (m_NumDrawCommands == 0) {
		// Release the vertex buffer allocated in beginFrame()
		releaseVertexBufferData_Vec2(m_VertexBuffers[0].m_Pos);
#if VG_CONFIG_UV_INT16
		releaseVertexBufferData_UV(m_VertexBuffers[0].m_UV);
#else
		releaseVertexBufferData_Vec2(m_VertexBuffers[0].m_UV);
#endif
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

		const bgfx::Memory* posMem = bgfx::makeRef(vb->m_Pos, sizeof(float) * 2 * vb->m_Count, releaseVertexBufferDataCallback_Vec2, this);
#if VG_CONFIG_UV_INT16
		const bgfx::Memory* uvMem = bgfx::makeRef(vb->m_UV, sizeof(int16_t) * 2 * vb->m_Count, releaseVertexBufferDataCallback_UV, this);
#else
		const bgfx::Memory* uvMem = bgfx::makeRef(vb->m_UV, sizeof(float) * 2 * vb->m_Count, releaseVertexBufferDataCallback_Vec2, this);
#endif
		const bgfx::Memory* colorMem = bgfx::makeRef(vb->m_Color, sizeof(uint32_t) * vb->m_Count, releaseVertexBufferDataCallback_Uint32, this);

		bgfx::updateDynamicVertexBuffer(vb->m_PosBufferHandle, 0, posMem);
		bgfx::updateDynamicVertexBuffer(vb->m_UVBufferHandle, 0, uvMem);
		bgfx::updateDynamicVertexBuffer(vb->m_ColorBufferHandle, 0, colorMem);

		vb->m_Pos = nullptr;
		vb->m_UV = nullptr;
		vb->m_Color = nullptr;
	}

	// Update bgfx index buffer...
	IndexBuffer* ib = m_ActiveIndexBuffer;
	const bgfx::Memory* indexMem = bgfx::makeRef(&ib->m_Indices[0], sizeof(uint16_t) * ib->m_Count, releaseIndexBufferCallback, this);
	if (!bgfx::isValid(ib->m_bgfxHandle)) {
		ib->m_bgfxHandle = bgfx::createDynamicIndexBuffer(indexMem, BGFX_BUFFER_ALLOW_RESIZE);
	} else {
		bgfx::updateDynamicIndexBuffer(ib->m_bgfxHandle, 0, indexMem);
	}
	
	float viewMtx[16];
	float projMtx[16];
	bx::mtxIdentity(viewMtx);
	bx::mtxOrtho(projMtx, 0.0f, m_WinWidth, m_WinHeight, 0.0f, 0.0f, 1.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
	bgfx::setViewTransform(m_ViewID, viewMtx, projMtx);

	uint16_t prevScissorRect[4] = { 0, 0, m_WinWidth, m_WinHeight };
	uint16_t prevScissorID = UINT16_MAX;
	uint32_t prevClipCmdID = UINT32_MAX;
	uint32_t stencilState = BGFX_STENCIL_NONE;
	uint8_t nextStencilValue = 1;

	const uint32_t numCommands = m_NumDrawCommands;
	for (uint32_t iCmd = 0; iCmd < numCommands; ++iCmd) {
		DrawCommand* cmd = &m_DrawCommands[iCmd];

		const ClipState* cmdClipState = &cmd->m_ClipState;
		if (cmdClipState->m_FirstCmdID != prevClipCmdID) {
			prevClipCmdID = cmdClipState->m_FirstCmdID;
			const uint32_t numClipCommands = cmdClipState->m_NumCmds;
			if (numClipCommands) {
				for (uint32_t iClip = 0; iClip < numClipCommands; ++iClip) {
					DrawCommand* clipCmd = &m_ClipCommands[cmdClipState->m_FirstCmdID + iClip];

					VertexBuffer* vb = &m_VertexBuffers[clipCmd->m_VertexBufferID];
					bgfx::setVertexBuffer(0, vb->m_PosBufferHandle, clipCmd->m_FirstVertexID, clipCmd->m_NumVertices);
					bgfx::setIndexBuffer(ib->m_bgfxHandle, clipCmd->m_FirstIndexID, clipCmd->m_NumIndices);

					// Set scissor.
					{
						const uint16_t* cmdScissorRect = &clipCmd->m_ScissorRect[0];
						if (!bx::memCmp(cmdScissorRect, &prevScissorRect[0], sizeof(uint16_t) * 4)) {
							// Re-set the previous cached scissor rect (submit() below doesn't preserve state).
							bgfx::setScissor(prevScissorID);
						} else {
							prevScissorID = bgfx::setScissor(cmdScissorRect[0], cmdScissorRect[1], cmdScissorRect[2], cmdScissorRect[3]);
							bx::memCopy(prevScissorRect, cmdScissorRect, sizeof(uint16_t) * 4);
						}
					}

					VG_CHECK(clipCmd->m_Type == DrawCommand::Type_Clip, "Invalid clip command");
					VG_CHECK(clipCmd->m_HandleID == UINT16_MAX, "Invalid clip command image handle");

					int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
					bgfx::setState(0);
					bgfx::setStencil(BGFX_STENCIL_TEST_ALWAYS     // pass always
						| BGFX_STENCIL_FUNC_REF(nextStencilValue) // value = nextStencilValue
						| BGFX_STENCIL_FUNC_RMASK(0xff)
						| BGFX_STENCIL_OP_FAIL_S_REPLACE
						| BGFX_STENCIL_OP_FAIL_Z_REPLACE
						| BGFX_STENCIL_OP_PASS_Z_REPLACE, BGFX_STENCIL_NONE);

					// TODO: Check if it's better to use Type_TexturedVertexColor program here to avoid too many 
					// state switches.
					bgfx::submit(m_ViewID, m_ProgramHandle[DrawCommand::Type_Clip], cmdDepth, false);
				}

				stencilState = (cmdClipState->m_Rule == ClipRule::In ? BGFX_STENCIL_TEST_EQUAL : BGFX_STENCIL_TEST_NOTEQUAL)
					| BGFX_STENCIL_FUNC_REF(nextStencilValue)
					| BGFX_STENCIL_FUNC_RMASK(0xff)
					| BGFX_STENCIL_OP_FAIL_S_KEEP
					| BGFX_STENCIL_OP_FAIL_Z_KEEP
					| BGFX_STENCIL_OP_PASS_Z_KEEP;

				++nextStencilValue;
			} else {
				stencilState = BGFX_STENCIL_NONE;
			}
		}

		VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
		bgfx::setVertexBuffer(0, vb->m_PosBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setVertexBuffer(1, vb->m_UVBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setVertexBuffer(2, vb->m_ColorBufferHandle, cmd->m_FirstVertexID, cmd->m_NumVertices);
		bgfx::setIndexBuffer(ib->m_bgfxHandle, cmd->m_FirstIndexID, cmd->m_NumIndices);

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
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid image handle");
			Image* tex = &m_Images[cmd->m_HandleID];
			bgfx::setTexture(0, m_TexUniform, tex->m_bgfxHandle, tex->m_Flags);

			int cmdDepth = 0; // TODO: Use depth to sort draw calls into layers.
			bgfx::setState(
				BGFX_STATE_ALPHA_WRITE |
				BGFX_STATE_RGB_WRITE |
				BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
			bgfx::setStencil(stencilState);

			bgfx::submit(m_ViewID, m_ProgramHandle[DrawCommand::Type_TexturedVertexColor], cmdDepth, false);
		} else if (cmd->m_Type == DrawCommand::Type_ColorGradient) {
			VG_CHECK(cmd->m_HandleID != UINT16_MAX, "Invalid gradient handle");
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
			bgfx::setStencil(stencilState);

			bgfx::submit(m_ViewID, m_ProgramHandle[DrawCommand::Type_ColorGradient], cmdDepth, false);
		} else {
			VG_CHECK(false, "Unknown draw command type");
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
	m_Stroker->reset(state->m_AvgScale, m_TesselationTolerance, m_FringeWidth);
	m_PathVerticesTransformed = false;
}

void Context::moveTo(float x, float y)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->moveTo(x, y);
}

void Context::lineTo(float x, float y)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->lineTo(x, y);
}

void Context::bezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->cubicTo(c1x, c1y, c2x, c2y, x, y);
}

void Context::arcTo(float x1, float y1, float x2, float y2, float radius)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	VG_WARN(false, "ArcTo() not implemented yet"); // NOT USED
	BX_UNUSED(x1, y1, x2, y2, radius);
}

void Context::rect(float x, float y, float w, float h)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->rect(x, y, w, h);
}

void Context::roundedRect(float x, float y, float w, float h, float r)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->roundedRect(x, y, w, h, r);
}

void Context::roundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->roundedRectVarying(x, y, w, h, rtl, rbl, rbr, rtr);
}

void Context::circle(float cx, float cy, float r)
{
	VG_CHECK(!m_PathVerticesTransformed, "Call beginPath() before starting a new path");
	m_Path->circle(cx, cy, r);
}

void Context::polyline(const float* coords, uint32_t numPoints)
{
	VG_CHECK(!m_PathVerticesTransformed, "Cannot add new vertices to the path after submitting a draw command");
	m_Path->polyline(coords, numPoints);
}

void Context::closePath()
{
	VG_CHECK(!m_PathVerticesTransformed, "Cannot add new vertices to the path after submitting a draw command");
	m_Path->close();
}

void Context::transformPath()
{
	if (m_PathVerticesTransformed) {
		return;
	}

	const uint32_t numPathVertices = m_Path->getNumVertices();
	if (numPathVertices > m_NumTransformedPathVertices) {
		m_TransformedPathVertices = (float*)BX_ALIGNED_REALLOC(m_Allocator, m_TransformedPathVertices, sizeof(float) * 2 * numPathVertices, 16);
		m_NumTransformedPathVertices = numPathVertices;
	}
	
	const float* pathVertices = m_Path->getVertices();
	const State* state = getState();
	batchTransformPositions(pathVertices, numPathVertices, m_TransformedPathVertices, state->m_TransformMtx);
	m_PathVerticesTransformed = true;
}

void Context::fillConvexPath(Color col, bool aa)
{
	if (m_RecordClipCommands) {
		col = ColorRGBA::Black;
		aa = false;
	}

	const State* state = getState();
	const uint32_t c = ColorRGBA::setAlpha(col, (uint8_t)(state->m_GlobalAlpha * ColorRGBA::getAlpha(col)));
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	transformPath();

	const float* pathVertices = m_TransformedPathVertices;

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t i = 0; i < numSubPaths; ++i) {
		const SubPath* path = &subPaths[i];
		if (path->m_NumVertices < 3) {
			continue;
		}

		const float* vtx = &pathVertices[path->m_FirstVertexID << 1];
		const uint32_t numPathVertices = path->m_NumVertices;

		Mesh mesh;
		const uint32_t* colors = &c;
		uint32_t numColors = 1;

		if (aa) {
			m_Stroker->convexFillAA(&mesh, vtx, numPathVertices, c);
			colors = mesh.m_ColorBuffer;
			numColors = mesh.m_NumVertices;
		} else {
			m_Stroker->convexFill(&mesh, vtx, numPathVertices);
		}

		if (m_RecordClipCommands) {
			createDrawCommand_Clip(mesh.m_PosBuffer, mesh.m_NumVertices, mesh.m_IndexBuffer, mesh.m_NumIndices);
		} else {
			createDrawCommand_VertexColor(mesh.m_PosBuffer, mesh.m_NumVertices, colors, numColors, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	}
}

void Context::fillConvexPath(GradientHandle gradientHandle, bool aa)
{
	VG_CHECK(!m_RecordClipCommands, "Only fillConvexPath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");

	// TODO: Anti-aliasing of gradient-filled paths
	BX_UNUSED(aa);

	transformPath();

	const float* pathVertices = m_TransformedPathVertices;

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t i = 0; i < numSubPaths; ++i) {
		const SubPath* path = &subPaths[i];
		if (path->m_NumVertices < 3) {
			continue;
		}

		const float* vtx = &pathVertices[path->m_FirstVertexID << 1];
		const uint32_t numPathVertices = path->m_NumVertices;

		Mesh mesh;
		m_Stroker->convexFill(&mesh, vtx, numPathVertices);
		createDrawCommand_Gradient(mesh.m_PosBuffer, mesh.m_NumVertices, gradientHandle, mesh.m_IndexBuffer, mesh.m_NumIndices);
	}
}

void Context::fillConvexPath(ImagePatternHandle imgHandle, bool aa)
{
	VG_CHECK(!m_RecordClipCommands, "Only fillConvexPath(Color) is supported inside BeginClip()/EndClip()");
	VG_CHECK(isValid(imgHandle), "Invalid image pattern handle");

	// TODO: Anti-aliasing of textured paths
	BX_UNUSED(aa);

	const State* state = getState();
	ImagePattern* img = &m_ImagePatterns[imgHandle.idx];
	uint32_t c = ColorRGBA::fromFloat(1.0f, 1.0f, 1.0f, img->m_Alpha * state->m_GlobalAlpha);
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	transformPath();

	const float* pathVertices = m_TransformedPathVertices;

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t i = 0; i < numSubPaths; ++i) {
		const SubPath* path = &subPaths[i];
		if (path->m_NumVertices < 3) {
			continue;
		}

		const float* vtx = &pathVertices[path->m_FirstVertexID << 1];
		const uint32_t numPathVertices = path->m_NumVertices;

		Mesh mesh;
		m_Stroker->convexFill(&mesh, vtx, numPathVertices);

		createDrawCommand_Textured(mesh.m_PosBuffer, mesh.m_NumVertices, &c, 1, img->m_ImageHandle, img->m_Matrix, mesh.m_IndexBuffer, mesh.m_NumIndices);
	}
}

void Context::fillConcavePath(Color col, bool aa)
{
	// TODO: AA
	if (m_RecordClipCommands) {
		col = ColorRGBA::Black;
		aa = false;
	}

	const State* state = getState();
	const uint32_t c = ColorRGBA::setAlpha(col, (uint8_t)(state->m_GlobalAlpha * ColorRGBA::getAlpha(col)));
	if (ColorRGBA::getAlpha(c) == 0) {
		return;
	}

	transformPath();

	const float* pathVertices = m_TransformedPathVertices;

	const uint32_t numSubPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	VG_CHECK(numSubPaths == 1, "Cannot decompose multiple concave paths");
	BX_UNUSED(numSubPaths);

	const SubPath* path = &subPaths[0];
	if (path->m_NumVertices < 3) {
		return;
	}

	Mesh mesh;
	if (m_Stroker->concaveFill(&mesh, &pathVertices[path->m_FirstVertexID << 1], path->m_NumVertices)) {
		if (m_RecordClipCommands) {
			createDrawCommand_Clip(mesh.m_PosBuffer, mesh.m_NumVertices, mesh.m_IndexBuffer, mesh.m_NumIndices);
		} else {
			createDrawCommand_VertexColor(mesh.m_PosBuffer, mesh.m_NumVertices, &c, 1, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	} else {
		VG_WARN(false, "Failed to triangulate concave polygon");
	}
}

void Context::strokePath(Color color, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	if (m_RecordClipCommands) {
		color = ColorRGBA::Black;
		aa = false;
	}

	transformPath();

	const float* pathVertices = m_TransformedPathVertices;

	const State* state = getState();
	const float avgScale = state->m_AvgScale;
	float strokeWidth = bx::clamp<float>(width * avgScale, 0.0f, 200.0f);
	float alphaScale = state->m_GlobalAlpha;
	bool isThin = false;
	if (strokeWidth <= m_FringeWidth) {
		float alpha = bx::clamp<float>(strokeWidth, 0.0f, m_FringeWidth);
		alphaScale *= alpha * alpha;
		strokeWidth = m_FringeWidth;
		isThin = true;
	}

	uint8_t newAlpha = (uint8_t)(alphaScale * ColorRGBA::getAlpha(color));
	if (newAlpha == 0) {
		return;
	}

	const uint32_t c = ColorRGBA::setAlpha(color, newAlpha);

	const uint32_t numPaths = m_Path->getNumSubPaths();
	const SubPath* subPaths = m_Path->getSubPaths();
	for (uint32_t iSubPath = 0; iSubPath < numPaths; ++iSubPath) {
		const SubPath* path = &subPaths[iSubPath];
		if (path->m_NumVertices < 2) {
			continue;
		}

		const float* vtx = &pathVertices[path->m_FirstVertexID << 1];
		const uint32_t numPathVertices = path->m_NumVertices;
		const bool isClosed = path->m_IsClosed;

		Mesh mesh;
		if (aa) {
			if (isThin) {
				m_Stroker->polylineStrokeAAThin(&mesh, vtx, numPathVertices, isClosed, c, lineCap, lineJoin);
			} else {
				m_Stroker->polylineStrokeAA(&mesh, vtx, numPathVertices, isClosed, c, strokeWidth, lineCap, lineJoin);
			}
		} else {
			m_Stroker->polylineStroke(&mesh, vtx, numPathVertices, isClosed, strokeWidth, lineCap, lineJoin);
		}

		if (m_RecordClipCommands) {
			createDrawCommand_Clip(mesh.m_PosBuffer, mesh.m_NumVertices, mesh.m_IndexBuffer, mesh.m_NumIndices);
		} else {
			const bool hasColors = mesh.m_ColorBuffer != nullptr;
			createDrawCommand_VertexColor(mesh.m_PosBuffer, mesh.m_NumVertices, hasColors ? mesh.m_ColorBuffer : &c, hasColors ? mesh.m_NumVertices : 1, mesh.m_IndexBuffer, mesh.m_NumIndices);
		}
	}
}

void Context::beginClip(ClipRule::Enum rule)
{
	m_ClipState.m_Rule = rule;
	m_ClipState.m_FirstCmdID = m_NumClipCommands;
	m_ClipState.m_NumCmds = 0;
	m_RecordClipCommands = true;
	m_ForceNewClipCommand = true;
}

void Context::endClip()
{
	VG_CHECK(m_RecordClipCommands, "Must be called once after beginClip");
	m_ClipState.m_NumCmds = m_NumClipCommands - m_ClipState.m_FirstCmdID;
	m_RecordClipCommands = false;
	m_ForceNewDrawCommand = true;
}

void Context::resetClip()
{
	VG_CHECK(!m_RecordClipCommands, "Must be called outside beginClip()/endClip() pair.");
	m_ClipState.m_FirstCmdID = ~0u;
	m_ClipState.m_NumCmds = 0;
	m_ForceNewDrawCommand = true;
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
#if 0
	float d = bx::sqrt(dx * dx + dy * dy);
#else
	float d = sqrtf(dx * dx + dy * dy);
#endif
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
	grad->m_Params[3] = bx::max<float>(1.0f, d);
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
	grad->m_Params[3] = bx::max<float>(1.0f, f);
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
	grad->m_Params[0] = r;
	grad->m_Params[1] = r;
	grad->m_Params[2] = r;
	grad->m_Params[3] = bx::max<float>(1.0f, f);
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

	const float cs = bx::cos(angle);
	const float sn = bx::sin(angle);

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
	VG_CHECK(m_CurStateID < MAX_STATE_STACK_SIZE - 1, "State stack overflow");
	bx::memCopy(&m_StateStack[m_CurStateID + 1], &m_StateStack[m_CurStateID], sizeof(State));
	++m_CurStateID;
}

void Context::popState()
{
	VG_CHECK(m_CurStateID > 0, "State stack underflow");
	--m_CurStateID;

	// If the new state has a different scissor rect than the last draw command 
	// force creating a new command.
	if (m_NumDrawCommands != 0) {
		const State* state = getState();
		const DrawCommand* lastDrawCommand = &m_DrawCommands[m_NumDrawCommands - 1];
		const uint16_t* lastScissor = &lastDrawCommand->m_ScissorRect[0];
		const float* stateScissor = &state->m_ScissorRect[0];
		if (lastScissor[0] != (uint16_t)stateScissor[0] ||
			lastScissor[1] != (uint16_t)stateScissor[1] ||
			lastScissor[2] != (uint16_t)stateScissor[2] ||
			lastScissor[3] != (uint16_t)stateScissor[3])
		{
			m_ForceNewDrawCommand = true;
			m_ForceNewClipCommand = true;
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
	m_ForceNewClipCommand = true;
}

void Context::setScissor(float x, float y, float w, float h)
{
	State* state = getState();
	float pos[2], size[2];
	transformPos2D(x, y, state->m_TransformMtx, &pos[0]);
	transformVec2D(w, h, state->m_TransformMtx, &size[0]);

	const float minx = bx::clamp<float>(pos[0], 0.0f, m_WinWidth);
	const float miny = bx::clamp<float>(pos[1], 0.0f, m_WinHeight);
	const float maxx = bx::clamp<float>(pos[0] + size[0], 0.0f, m_WinWidth);
	const float maxy = bx::clamp<float>(pos[1] + size[1], 0.0f, m_WinHeight);

	state->m_ScissorRect[0] = minx;
	state->m_ScissorRect[1] = miny;
	state->m_ScissorRect[2] = maxx - minx;
	state->m_ScissorRect[3] = maxy - miny;
	m_ForceNewDrawCommand = true;
	m_ForceNewClipCommand = true;
}

bool Context::intersectScissor(float x, float y, float w, float h)
{
	State* state = getState();
	float pos[2], size[2];
	transformPos2D(x, y, state->m_TransformMtx, &pos[0]);
	transformVec2D(w, h, state->m_TransformMtx, &size[0]);

	const float* rect = state->m_ScissorRect;

	float minx = bx::max<float>(pos[0], rect[0]);
	float miny = bx::max<float>(pos[1], rect[1]);
	float maxx = bx::min<float>(pos[0] + size[0], rect[0] + rect[2]);
	float maxy = bx::min<float>(pos[1] + size[1], rect[1] + rect[3]);

	state->m_ScissorRect[0] = minx;
	state->m_ScissorRect[1] = miny;
	state->m_ScissorRect[2] = bx::max<float>(0.0f, maxx - minx);
	state->m_ScissorRect[3] = bx::max<float>(0.0f, maxy - miny);

	m_ForceNewDrawCommand = true;
	m_ForceNewClipCommand = true;

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
	const float c = bx::cos(ang_rad);
	const float s = bx::sin(ang_rad);

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
		m_TextVertices = (float*)BX_ALIGNED_REALLOC(m_Allocator, m_TextVertices, sizeof(float) * 2 * m_TextQuadCapacity * 4, 16);
	}

	bx::memCopy(m_TextQuads, m_TextString.m_Quads, sizeof(FONSquad) * numBakedChars);

	float dx = 0.0f, dy = 0.0f;
	fonsAlignString(m_FontStashContext, &m_TextString, alignment, &dx, &dy);

	pushState();
	transformTranslate(x + dx / scale, y + dy / scale);
	renderTextQuads(numBakedChars, color);
	popState();
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

			minx = bx::min<float>(minx, rminx);
			maxx = bx::max<float>(maxx, rmaxx);

			// Vertical bounds.
			miny = bx::min<float>(miny, y + rminy);
			maxy = bx::max<float>(maxy, y + rmaxy);

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
		positions[npos].minx = bx::min<float>(iter.x, q.x0) * invscale;
		positions[npos].maxx = bx::max<float>(iter.nextx, q.x1) * invscale;
		
		npos++;
		if (npos >= maxPositions) {
			break;
		}
	}

	return npos;
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

	bx::MemoryReader cmdListReader(shape->m_CmdList->more(0), (uint32_t)shape->m_CmdListWriter.seek(0, bx::Whence::Current));

	const uint16_t firstGradientID = (uint16_t)m_NextGradientID;
	const uint16_t firstImagePatternID = (uint16_t)m_NextImagePatternID;
	VG_CHECK(firstGradientID + shape->m_NumGradients <= VG_CONFIG_MAX_GRADIENTS, "Not enough free gradients to render shape. Increase VG_MAX_GRADIENTS");
	VG_CHECK(firstImagePatternID + shape->m_NumImagePatterns <= VG_CONFIG_MAX_IMAGE_PATTERNS, "Not enough free image patterns to render shape. Increase VG_MAX_IMAGE_PATTERS");

	bool skipCmds = false;
	pushState();
	while (cmdListReader.remaining()) {
		ShapeCommand::Enum cmdType;
		bx::read(&cmdListReader, cmdType);

		switch (cmdType) {
		case ShapeCommand::BeginPath:
		{
			if (!skipCmds) {
				beginPath();
			}
			break;
		}
		case ShapeCommand::ClosePath:
		{
			if (!skipCmds) {
				closePath();
			}
			break;
		}
		case ShapeCommand::MoveTo:
		{
			float coords[2];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 2);
			if (!skipCmds) {
				moveTo(coords[0], coords[1]);
			}
			break;
		}
		case ShapeCommand::LineTo:
		{
			float coords[2];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 2);
			if (!skipCmds) {
				lineTo(coords[0], coords[1]);
			}
			break;
		}
		case ShapeCommand::BezierTo:
		{
			float coords[6];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 6);
			if (!skipCmds) {
				bezierTo(coords[0], coords[1], coords[2], coords[3], coords[4], coords[5]);
			}
			break;
		}
		case ShapeCommand::ArcTo:
		{
			float coords[5];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 5);
			if (!skipCmds) {
				arcTo(coords[0], coords[1], coords[2], coords[3], coords[4]);
			}
			break;
		}
		case ShapeCommand::Rect:
		{
			float coords[4];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 4);
			if (!skipCmds) {
				rect(coords[0], coords[1], coords[2], coords[3]);
			}
			break;
		}
		case ShapeCommand::RoundedRect:
		{
			float coords[5];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 5);
			if (!skipCmds) {
				roundedRect(coords[0], coords[1], coords[2], coords[3], coords[4]);
			}
			break;
		}
		case ShapeCommand::RoundedRectVarying:
		{
			float coords[8];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 8);
			if (!skipCmds) {
				roundedRectVarying(coords[0], coords[1], coords[2], coords[3], coords[4], coords[5], coords[6], coords[7]);
			}
			break;
		}
		case ShapeCommand::Circle:
		{
			float coords[3];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 3);
			if (!skipCmds) {
				circle(coords[0], coords[1], coords[2]);
			}
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

			Color col;
			bool aa;
			bx::read(&cmdListReader, col);
			bx::read(&cmdListReader, aa);
			if (!skipCmds) {
				fillConvexPath(col, aa);
			}

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
			GradientHandle handle;
			bool aa;

			bx::read(&cmdListReader, handle);
			bx::read(&cmdListReader, aa);

			handle.idx += firstGradientID;
			if (!skipCmds) {
				fillConvexPath(handle, aa);
			}
			break;
		}
		case ShapeCommand::FillConvexImage:
		{
			// TODO: Cache image fills
			ImagePatternHandle handle;
			bool aa;

			bx::read(&cmdListReader, handle);
			bx::read(&cmdListReader, aa);

			handle.idx += firstImagePatternID;
			if (!skipCmds) {
				fillConvexPath(handle, aa);
			}
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

			Color col;
			bool aa;

			bx::read(&cmdListReader, col);
			bx::read(&cmdListReader, aa);

			if (!skipCmds) {
				fillConcavePath(col, aa);
			}

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
			Color col;
			float width;
			bool aa;
			LineCap::Enum lineCap;
			LineJoin::Enum lineJoin;

			bx::read(&cmdListReader, col);
			bx::read(&cmdListReader, width);
			bx::read(&cmdListReader, aa);
			bx::read(&cmdListReader, lineCap);
			bx::read(&cmdListReader, lineJoin);

			if (!skipCmds) {
				strokePath(col, width, aa, lineCap, lineJoin);
			}

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
			float grad[4];
			Color col[2];
			bx::read(&cmdListReader, &grad[0], sizeof(float) * 4);
			bx::read(&cmdListReader, &col[0], sizeof(Color) * 2);
			createLinearGradient(grad[0], grad[1], grad[2], grad[3], col[0], col[1]);
			break;
		}
		case ShapeCommand::BoxGradient:
		{
			float grad[6];
			Color col[2];
			bx::read(&cmdListReader, &grad[0], sizeof(float) * 6);
			bx::read(&cmdListReader, &col[0], sizeof(Color) * 2);
			createBoxGradient(grad[0], grad[1], grad[2], grad[3], grad[4], grad[5], col[0], col[1]);
			break;
		}
		case ShapeCommand::RadialGradient:
		{
			float grad[4];
			Color col[2];
			bx::read(&cmdListReader, &grad[0], sizeof(float) * 4);
			bx::read(&cmdListReader, &col[0], sizeof(Color) * 2);
			createRadialGradient(grad[0], grad[1], grad[2], grad[3], col[0], col[1]);
			break;
		}
		case ShapeCommand::ImagePattern:
		{
			float params[6];
			ImageHandle image;
			bx::read(&cmdListReader, &params[0], sizeof(float) * 6);
			bx::read(&cmdListReader, image);
			createImagePattern(params[0], params[1], params[2], params[3], params[4], image, params[5]);
			break;
		}
		case ShapeCommand::TextStatic:
		{
//#if VG_CONFIG_ENABLE_SHAPE_CACHING
//			if (canCache) {
//				cachedShapeAddTextCommand(cachedShape, cmdList - sizeof(ShapeCommand::Enum));
//			}
//#endif
			ShapeCommandText textCmd;
			bx::read(&cmdListReader, textCmd);
			
			const char* str = (const char*)cmdListReader.getDataPtr();
			cmdListReader.seek(textCmd.len, bx::Whence::Current);

#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				String* cachedString = createString(textCmd.font, str, str + textCmd.len);
				cachedShapeAddTextCommand(cachedShape, cachedString, textCmd.alignment, textCmd.col, textCmd.x, textCmd.y);
				if (!skipCmds) {
					text(cachedString, textCmd.alignment, textCmd.col, textCmd.x, textCmd.y);
				}
			} else {
				if (!skipCmds) {
					text(textCmd.font, textCmd.alignment, textCmd.col, textCmd.x, textCmd.y, str, str + textCmd.len);
				}
			}
#else
			text(textCmd.font, textCmd.alignment, textCmd.col, textCmd.x, textCmd.y, str, str + textCmd.len);
#endif
			break;
		}
#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
		case ShapeCommand::TextDynamic:
		{
#if VG_CONFIG_ENABLE_SHAPE_CACHING
			if (canCache) {
				cachedShapeAddTextCommand(cachedShape, cmdListReader.getDataPtr() - sizeof(ShapeCommand::Enum));
			}
#endif
			ShapeCommandText textCmd;
			bx::read(&cmdListReader, textCmd);

			VG_WARN(stringCallback, "Shape includes dynamic text commands but no string callback has been specified");
			if (stringCallback) {
				uint32_t len;
				const char* str = (*stringCallback)(textCmd.len, len, userData);
				const char* end = len != ~0u ? str + len : str + bx::strLen(str);
				if (!skipCmds) {
					text(textCmd.font, textCmd.alignment, textCmd.col, textCmd.x, textCmd.y, str, end);
				}
			}

			break;
		}
#endif
		case ShapeCommand::Scissor:
		{
			float rect[4];
			bx::read(&cmdListReader, &rect[0], sizeof(float) * 4);

			setScissor(rect[0], rect[1], rect[2], rect[3]);
			skipCmds = false;
			break;
		}
		case ShapeCommand::IntersectScissor:
		{
			float rect[4];
			bx::read(&cmdListReader, &rect[0], sizeof(float) * 4);

			// TODO: Get the result and "cull" the following draw commands if it's false.
			// Problem is I don't know how it will behave with cached shapes.
			skipCmds = !intersectScissor(rect[0], rect[1], rect[2], rect[3]);
			break;
		}
		case ShapeCommand::PushState:
			pushState();
			break;
		case ShapeCommand::PopState:
			popState();
			skipCmds = false;
			break;
		case ShapeCommand::Rotate:
		{
			float ang_rad;
			bx::read(&cmdListReader, ang_rad);
			transformRotate(ang_rad);
			break;
		}
		case ShapeCommand::Translate:
		{
			float coords[2];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 2);
			transformTranslate(coords[0], coords[1]);
			break;
		}
		case ShapeCommand::Scale:
		{
			float coords[2];
			bx::read(&cmdListReader, &coords[0], sizeof(float) * 2);
			transformScale(coords[0], coords[1]);
			break;
		}
		case ShapeCommand::BeginClip:
		{
			ClipRule::Enum rule;
			bx::read(&cmdListReader, rule);
			beginClip(rule);
			break;
		}
		case ShapeCommand::EndClip:
		{
			endClip();
			break;
		}
		case ShapeCommand::ResetClip:
		{
			resetClip();
			break;
		}
		default:
			VG_CHECK(false, "Unknown shape command");
			break;
		}
	}
	popState();
	resetClip();

	// TODO: Remove this. Don't free gradients or images because handles should last the whole frame.
//	// Free shape gradients and image patterns
//	m_NextGradientID = firstGradientID;
//	m_NextImagePatternID = firstImagePatternID;
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
		IndexBuffer* ib = m_ActiveIndexBuffer;
		const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;
		float* dstPos = &vb->m_Pos[vbOffset << 1];
#if VG_CONFIG_UV_INT16
		int16_t* dstUV = &vb->m_UV[vbOffset << 1];
#else
		float* dstUV = &vb->m_UV[vbOffset << 1];
#endif
		uint32_t* dstColor = &vb->m_Color[vbOffset];
		uint16_t* dstIndex = &ib->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];

		const uint16_t startVertexID = (uint16_t)cmd->m_NumVertices;

		batchTransformPositions_Unaligned(cachedCmd->m_Pos, cachedCmd->m_NumVertices, dstPos, mtx);
#if VG_CONFIG_UV_INT16
		bx::memCopy(dstUV, cachedCmd->m_UV, sizeof(int16_t) * 2 * cachedCmd->m_NumVertices);
#else
		bx::memCopy(dstUV, cachedCmd->m_UV, sizeof(float) * 2 * cachedCmd->m_NumVertices);
#endif
		bx::memCopy(dstColor, cachedCmd->m_Color, sizeof(uint32_t) * cachedCmd->m_NumVertices);
		batchTransformDrawIndices(cachedCmd->m_Indices, cachedCmd->m_NumIndices, dstIndex, startVertexID);

		cmd->m_NumVertices += cachedCmd->m_NumVertices;
		cmd->m_NumIndices += cachedCmd->m_NumIndices;
	}

	// Render all text commands...
	const uint32_t numStaticTextCommands = shape->m_NumStaticTextCommands;
	for (uint32_t iStatic = 0; iStatic < numStaticTextCommands; ++iStatic) {
		CachedTextCommand* cmd = &shape->m_StaticTextCommands[iStatic];
		text(cmd->m_Text, cmd->m_Alignment, cmd->m_Color, cmd->m_Pos[0], cmd->m_Pos[1]);
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

			VG_WARN(stringCallback, "Shape includes dynamic text commands but not string callback has been specified");
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
			VG_CHECK(false, "Unknown shape command");
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
	VG_CHECK(cmd->m_NumVertices < 65536, "Each draw command should have max 65535 vertices");
	VG_CHECK(cmd->m_NumIndices < 65536, "Each draw command should have max 65535 indices");
	VG_CHECK(cmd->m_Type == DrawCommand::Type_TexturedVertexColor && cmd->m_HandleID == m_FontImages[0].idx, "Cannot cache draw command");

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

	VG_CHECK(cachedCmd->m_NumVertices + cmd->m_NumVertices < 65536, "Not enough space in current cached command");
	VG_CHECK(cachedCmd->m_NumIndices + cmd->m_NumIndices < 65536, "Not enough space in current cached command");
	cachedCmd->m_NumVertices += (uint16_t)cmd->m_NumVertices;
	cachedCmd->m_Pos = (float*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_Pos, sizeof(float) * 2 * cachedCmd->m_NumVertices, 16);
#if VG_CONFIG_UV_INT16
	cachedCmd->m_UV = (int16_t*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_UV, sizeof(int16_t) * 2 * cachedCmd->m_NumVertices, 16);
#else
	cachedCmd->m_UV = (float*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_UV, sizeof(float) * 2 * cachedCmd->m_NumVertices, 16);
#endif
	cachedCmd->m_Color = (uint32_t*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_Color, sizeof(uint32_t) * cachedCmd->m_NumVertices, 16);

	const float* srcPos = &m_VertexBuffers[cmd->m_VertexBufferID].m_Pos[cmd->m_FirstVertexID << 1];
#if VG_CONFIG_UV_INT16
	const int16_t* srcUV = &m_VertexBuffers[cmd->m_VertexBufferID].m_UV[cmd->m_FirstVertexID << 1];
#else
	const float* srcUV = &m_VertexBuffers[cmd->m_VertexBufferID].m_UV[cmd->m_FirstVertexID << 1];
#endif
	const uint32_t* srcColor = &m_VertexBuffers[cmd->m_VertexBufferID].m_Color[cmd->m_FirstVertexID];
	bx::memCopy(&cachedCmd->m_Pos[firstVertexID << 1], srcPos, sizeof(float) * 2 * cmd->m_NumVertices);
#if VG_CONFIG_UV_INT16
	bx::memCopy(&cachedCmd->m_UV[firstVertexID << 1], srcUV, sizeof(int16_t) * 2 * cmd->m_NumVertices);
#else
	bx::memCopy(&cachedCmd->m_UV[firstVertexID << 1], srcUV, sizeof(float) * 2 * cmd->m_NumVertices);
#endif
	bx::memCopy(&cachedCmd->m_Color[firstVertexID], srcColor, sizeof(uint32_t) * cmd->m_NumVertices);

	// Copy the indices...
	cachedCmd->m_NumIndices += (uint16_t)cmd->m_NumIndices;
	cachedCmd->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, cachedCmd->m_Indices, sizeof(uint16_t) * cachedCmd->m_NumIndices, 16);

	const uint16_t* srcIndex = &m_ActiveIndexBuffer->m_Indices[cmd->m_FirstIndexID];
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
	cmd->m_Pos[0] = x;
	cmd->m_Pos[1] = y;
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
		VG_CHECK(iw != -1 && ih != -1, "Invalid font atlas dimensions");

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

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, m_TextVertices, sizeof(float) * 2 * numDrawVertices);

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numDrawVertices, &c);

#if VG_CONFIG_UV_INT16
	int16_t* dstUV = &vb->m_UV[vbOffset << 1];
	const FONSquad* q = m_TextQuads;
	uint32_t nq = numQuads;
	while (nq-- > 0) {
		const float s0 = q->s0;
		const float s1 = q->s1;
		const float t0 = q->t0;
		const float t1 = q->t1;

		dstUV[0] = (int16_t)(s0 * INT16_MAX); dstUV[1] = (int16_t)(t0 * INT16_MAX);
		dstUV[2] = (int16_t)(s1 * INT16_MAX); dstUV[3] = (int16_t)(t0 * INT16_MAX);
		dstUV[4] = (int16_t)(s1 * INT16_MAX); dstUV[5] = (int16_t)(t1 * INT16_MAX);
		dstUV[6] = (int16_t)(s0 * INT16_MAX); dstUV[7] = (int16_t)(t1 * INT16_MAX);

		dstUV += 8;
		++q;
	}
#else
	float* dstUV = &vb->m_UV[vbOffset << 1];
	const FONSquad* q = m_TextQuads;
	uint32_t nq = numQuads;
	while (nq-- > 0) {
		const float s0 = q->s0;
		const float s1 = q->s1;
		const float t0 = q->t0;
		const float t1 = q->t1;

		dstUV[0] = s0; dstUV[1] = t0;
		dstUV[2] = s1; dstUV[3] = t0;
		dstUV[4] = s1; dstUV[5] = t1;
		dstUV[6] = s0; dstUV[7] = t1;

		dstUV += 8;
		++q;
	}
#endif

	uint16_t* dstIndex = &m_ActiveIndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	genQuadIndices_unaligned(dstIndex, numQuads, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numDrawVertices;
	cmd->m_NumIndices += numDrawIndices;
}

DrawCommand* Context::allocDrawCommand_TexturedVertexColor(uint32_t numVertices, uint32_t numIndices, ImageHandle img)
{
	VG_CHECK(isValid(img), "Invalid image handle");

	State* state = getState();
	const float* scissor = state->m_ScissorRect;

	VertexBuffer* vb = getVertexBuffer();
	if (vb->m_Count + numVertices > MAX_VB_VERTICES) {
		vb = allocVertexBuffer();

		// The currently active vertex buffer has changed so force a new draw command.
		m_ForceNewDrawCommand = true;
	}

	uint32_t vbID = (uint32_t)(vb - m_VertexBuffers);

	IndexBuffer* ib = m_ActiveIndexBuffer;
	if (ib->m_Count + numIndices > ib->m_Capacity) {
		const uint32_t nextCapacity = ib->m_Capacity != 0 ? (ib->m_Capacity * 3) / 2 : 32;
		ib->m_Capacity = bx::uint32_max(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity, 16);
	}

	if (!m_ForceNewDrawCommand && m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &m_DrawCommands[m_NumDrawCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vbID, "Cannot merge draw commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] 
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
	bx::memCopy(&cmd->m_ClipState, &m_ClipState, sizeof(ClipState));

	vb->m_Count += numVertices;
	ib->m_Count += numIndices;

	m_ForceNewDrawCommand = false;

	return cmd;
}

DrawCommand* Context::allocDrawCommand_ColorGradient(uint32_t numVertices, uint32_t numIndices, GradientHandle gradientHandle)
{
	VG_CHECK(isValid(gradientHandle), "Invalid gradient handle");

	State* state = getState();
	const float* scissor = state->m_ScissorRect;

	VertexBuffer* vb = getVertexBuffer();
	if (vb->m_Count + numVertices > MAX_VB_VERTICES) {
		vb = allocVertexBuffer();

		// The currently active vertex buffer has changed so force a new draw command.
		m_ForceNewDrawCommand = true;
	}

	uint32_t vbID = (uint32_t)(vb - m_VertexBuffers);

	IndexBuffer* ib = m_ActiveIndexBuffer;
	if (ib->m_Count + numIndices > ib->m_Capacity) {
		const uint32_t nextCapacity = ib->m_Capacity != 0 ? (ib->m_Capacity * 3) / 2 : 32;
		ib->m_Capacity = bx::uint32_max(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity, 16);
	}

	if (!m_ForceNewDrawCommand && m_NumDrawCommands != 0) {
		DrawCommand* prevCmd = &m_DrawCommands[m_NumDrawCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vbID, "Cannot merge draw commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0] 
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
	bx::memCopy(&cmd->m_ClipState, &m_ClipState, sizeof(ClipState));

	vb->m_Count += numVertices;
	ib->m_Count += numIndices;

	m_ForceNewDrawCommand = false;

	return cmd;
}

DrawCommand* Context::allocDrawCommand_Clip(uint32_t numVertices, uint32_t numIndices)
{
	State* state = getState();
	const float* scissor = state->m_ScissorRect;

	VertexBuffer* vb = getVertexBuffer();
	if (vb->m_Count + numVertices > MAX_VB_VERTICES) {
		vb = allocVertexBuffer();

		// The currently active vertex buffer has changed so force a new draw command.
		m_ForceNewClipCommand = true;
	}

	uint32_t vbID = (uint32_t)(vb - m_VertexBuffers);

	IndexBuffer* ib = m_ActiveIndexBuffer;
	if (ib->m_Count + numIndices > ib->m_Capacity) {
		const uint32_t nextCapacity = ib->m_Capacity != 0 ? (ib->m_Capacity * 3) / 2 : 32;
		ib->m_Capacity = bx::uint32_max(nextCapacity, ib->m_Count + numIndices);
		ib->m_Indices = (uint16_t*)BX_ALIGNED_REALLOC(m_Allocator, ib->m_Indices, sizeof(uint16_t) * ib->m_Capacity, 16);
	}

	if (!m_ForceNewClipCommand && m_NumClipCommands != 0) {
		DrawCommand* prevCmd = &m_ClipCommands[m_NumClipCommands - 1];

		VG_CHECK(prevCmd->m_VertexBufferID == vbID, "Cannot merge clip commands with different vertex buffers");
		VG_CHECK(prevCmd->m_ScissorRect[0] == (uint16_t)scissor[0]
			&& prevCmd->m_ScissorRect[1] == (uint16_t)scissor[1]
			&& prevCmd->m_ScissorRect[2] == (uint16_t)scissor[2]
			&& prevCmd->m_ScissorRect[3] == (uint16_t)scissor[3], "Invalid scissor rect");
		VG_CHECK(prevCmd->m_Type == DrawCommand::Type_Clip, "Invalid draw command type");

		vb->m_Count += numVertices;
		ib->m_Count += numIndices;
		return prevCmd;
	}

	// If we land here it means that the current draw command cannot be batched with the previous command.
	// Create a new one.
	if (m_NumClipCommands + 1 >= m_ClipCommandCapacity) {
		m_ClipCommandCapacity = m_ClipCommandCapacity + 32;
		m_ClipCommands = (DrawCommand*)BX_REALLOC(m_Allocator, m_ClipCommands, sizeof(DrawCommand) * m_ClipCommandCapacity);
	}

	DrawCommand* cmd = &m_ClipCommands[m_NumClipCommands++];
	cmd->m_VertexBufferID = vbID;
	cmd->m_FirstVertexID = vb->m_Count;
	cmd->m_FirstIndexID = ib->m_Count;
	cmd->m_NumVertices = 0;
	cmd->m_NumIndices = 0;
	cmd->m_Type = DrawCommand::Type_Clip;
	cmd->m_HandleID = UINT16_MAX;
	cmd->m_ScissorRect[0] = (uint16_t)scissor[0];
	cmd->m_ScissorRect[1] = (uint16_t)scissor[1];
	cmd->m_ScissorRect[2] = (uint16_t)scissor[2];
	cmd->m_ScissorRect[3] = (uint16_t)scissor[3];
	cmd->m_ClipState.m_FirstCmdID = ~0u;
	cmd->m_ClipState.m_NumCmds = 0;

	vb->m_Count += numVertices;
	ib->m_Count += numIndices;

	m_ForceNewClipCommand = false;

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
#if VG_CONFIG_UV_INT16
	vb->m_UV = allocVertexBufferData_UV();
#else
	vb->m_UV = allocVertexBufferData_Vec2();
#endif
	vb->m_Color = allocVertexBufferData_Uint32();
	vb->m_Count = 0;

	return vb;
}

IndexBuffer* Context::allocIndexBuffer()
{
	IndexBuffer* ib = nullptr;
	for (uint32_t i = 0; i < m_NumIndexBuffers; ++i) {
		if (m_IndexBuffers[i].m_Count == 0) {
			ib = &m_IndexBuffers[i];
			break;
		}
	}

	if (!ib) {
		m_NumIndexBuffers++;
		m_IndexBuffers = (IndexBuffer*)BX_REALLOC(m_Allocator, m_IndexBuffers, sizeof(IndexBuffer) * m_NumIndexBuffers);

		ib = &m_IndexBuffers[m_NumIndexBuffers - 1];
		ib->m_bgfxHandle = BGFX_INVALID_HANDLE;
		ib->m_Capacity = 0;
		ib->m_Count = 0;
		ib->m_Indices = nullptr;
	}

	return ib;
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

	VG_CHECK(handle.idx < m_ImageCapacity, "Allocated invalid image handle");
	Image* tex = &m_Images[handle.idx];
	VG_CHECK(!bgfx::isValid(tex->m_bgfxHandle), "Allocated texture is already in use");
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
	VG_CHECK(iw > 0 && ih > 0, "Invalid font atlas dimensions");

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
	VG_CHECK(bgfx::isValid(tex->m_bgfxHandle), "Invalid texture handle");

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
	VG_CHECK(bgfx::isValid(tex->m_bgfxHandle), "Invalid texture handle");
	bgfx::destroy(tex->m_bgfxHandle);
	tex->reset();
	
	m_ImageAlloc.free(img.idx);

	return true;
}

float* Context::allocVertexBufferData_Vec2()
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	for (uint32_t i = 0; i < m_Vec2DataPoolCapacity; ++i) {
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)m_Vec2DataPool[i]) & 1) {
			// Remove the free flag
			m_Vec2DataPool[i] = (float*)((uintptr_t)m_Vec2DataPool[i] & ~1);
			return m_Vec2DataPool[i];
		} else if (m_Vec2DataPool[i] == nullptr) {
			m_Vec2DataPool[i] = (float*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(float) * 2 * MAX_VB_VERTICES, 16);
			return m_Vec2DataPool[i];
		}
	}

	uint32_t oldCapacity = m_Vec2DataPoolCapacity;
	m_Vec2DataPoolCapacity += 8;
	m_Vec2DataPool = (float**)BX_REALLOC(m_Allocator, m_Vec2DataPool, sizeof(float*) * m_Vec2DataPoolCapacity);
	bx::memSet(&m_Vec2DataPool[oldCapacity], 0, sizeof(float*) * (m_Vec2DataPoolCapacity - oldCapacity));

	m_Vec2DataPool[oldCapacity] = (float*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(float) * 2 * MAX_VB_VERTICES, 16);
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

#if VG_CONFIG_UV_INT16
int16_t* Context::allocVertexBufferData_UV()
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	for (uint32_t i = 0; i < m_UVDataPoolCapacity; ++i) {
		// If LSB of pointer is set it means that the ptr is valid and the buffer is free for reuse.
		if (((uintptr_t)m_UVDataPool[i]) & 1) {
			// Remove the free flag
			m_UVDataPool[i] = (int16_t*)((uintptr_t)m_UVDataPool[i] & ~1);
			return m_UVDataPool[i];
		} else if (m_UVDataPool[i] == nullptr) {
			m_UVDataPool[i] = (int16_t*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(int16_t) * 2 * MAX_VB_VERTICES, 16);
			return m_UVDataPool[i];
		}
	}

	uint32_t oldCapacity = m_UVDataPoolCapacity;
	m_UVDataPoolCapacity += 8;
	m_UVDataPool = (int16_t**)BX_REALLOC(m_Allocator, m_UVDataPool, sizeof(int16_t*) * m_UVDataPoolCapacity);
	bx::memSet(&m_UVDataPool[oldCapacity], 0, sizeof(int16_t*) * (m_UVDataPoolCapacity - oldCapacity));

	m_UVDataPool[oldCapacity] = (int16_t*)BX_ALIGNED_ALLOC(m_Allocator, sizeof(int16_t) * 2 * MAX_VB_VERTICES, 16);
	return m_UVDataPool[oldCapacity];
}
#endif

void Context::releaseVertexBufferData_Vec2(float* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	for (uint32_t i = 0; i < m_Vec2DataPoolCapacity; ++i) {
		if (m_Vec2DataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			m_Vec2DataPool[i] = (float*)((uintptr_t)m_Vec2DataPool[i] | 1);
			return;
		}
	}
}

void Context::releaseVertexBufferData_Uint32(uint32_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	for (uint32_t i = 0; i < m_Uint32DataPoolCapacity; ++i) {
		if (m_Uint32DataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			m_Uint32DataPool[i] = (uint32_t*)((uintptr_t)m_Uint32DataPool[i] | 1);
			return;
		}
	}
}

#if VG_CONFIG_UV_INT16
void Context::releaseVertexBufferData_UV(int16_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	for (uint32_t i = 0; i < m_UVDataPoolCapacity; ++i) {
		if (m_UVDataPool[i] == data) {
			// Mark buffer as free by setting the LSB of the ptr to 1.
			m_UVDataPool[i] = (int16_t*)((uintptr_t)m_UVDataPool[i] | 1);
			return;
		}
	}
}
#endif

void Context::releaseIndexBuffer(uint16_t* data)
{
#if BX_CONFIG_SUPPORTS_THREADING
	bx::MutexScope ms(m_DataPoolMutex);
#endif

	VG_CHECK(data != nullptr, "Tried to release a null vertex buffer");
	for (uint32_t i = 0; i < m_NumIndexBuffers; ++i) {
		if (m_IndexBuffers[i].m_Indices == data) {
			// Reset the ib for reuse.
			m_IndexBuffers[i].m_Count = 0;
			return;
		}
	}
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
		m_TextVertices = (float*)BX_ALIGNED_REALLOC(m_Allocator, m_TextVertices, sizeof(float) * 2 * m_TextQuadCapacity * 4, 16);
	}

	bx::memCopy(m_TextQuads, fs->m_Quads, sizeof(FONSquad) * numBakedChars);

	float dx = 0.0f, dy = 0.0f;
	fonsAlignString(m_FontStashContext, fs, alignment, &dx, &dy);

	pushState();
	transformTranslate(x + dx / scale, y + dy / scale);
	renderTextQuads(numBakedChars, color);
	popState();
}

void Context::createDrawCommand_VertexColor(const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, const uint16_t* indices, uint32_t numIndices)
{
	// Allocate the draw command
	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numVertices, numIndices, m_FontImages[0]);

	// Vertex buffer
	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

#if VG_CONFIG_UV_INT16
	int16_t uv[2];
	getWhitePixelUV(&uv[0]);

	int16_t* dstUV = &vb->m_UV[vbOffset << 1];
	memset32(dstUV, numVertices, &uv[0]);
#else
	float uv[2];
	getWhitePixelUV(&uv[0]);

	float* dstUV = &vb->m_UV[vbOffset << 1];
	memset64(dstUV, numVertices, &uv[0]);
#endif

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	if (numColors == numVertices) {
		bx::memCopy(dstColor, colors, sizeof(uint32_t) * numVertices);
	} else {
		VG_CHECK(numColors == 1, "!!!");
		memset32(dstColor, numVertices, colors);
	}

	// Index buffer
	uint16_t* dstIndex = &m_ActiveIndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

void Context::createDrawCommand_Textured(const float* vtx, uint32_t numVertices, const uint32_t* colors, uint32_t numColors, ImageHandle imgHandle, const float* uvMatrix, const uint16_t* indices, uint32_t numIndices)
{
	DrawCommand* cmd = allocDrawCommand_TexturedVertexColor(numVertices, numIndices, imgHandle);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

#if VG_CONFIG_UV_INT16
	int16_t* dstUV = &vb->m_UV[vbOffset << 1];
	generateUVs(vtx, numVertices, dstUV, uvMatrix);
#else
	float* dstUV = &vb->m_UV[vbOffset << 1];
	batchTransformPositions_Unaligned(vtx, numVertices, dstUV, uvMatrix);
#endif

	uint32_t* dstColor = &vb->m_Color[vbOffset];
	if (numColors == numVertices) {
		bx::memCopy(dstColor, colors, sizeof(uint32_t) * numVertices);
	} else {
		VG_CHECK(numColors == 1, "Number of colors should either be 1 or match the number of vertices");
		memset32(dstColor, numVertices, colors);
	}

	uint16_t* dstIndex = &m_ActiveIndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

void Context::createDrawCommand_Gradient(const float* vtx, uint32_t numVertices, GradientHandle gradientHandle, const uint16_t* indices, uint32_t numIndices)
{
	DrawCommand* cmd = allocDrawCommand_ColorGradient(numVertices, numIndices, gradientHandle);

	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

#if 0
#if VG_CONFIG_UV_INT16
	int16_t* dstUV = &vb->m_UV[vbOffset << 1];
	bx::memSet(dstUV, 0, sizeof(int16_t) * 2 * numVertices);
#else
	float* dstUV = &vb->m_UV[vbOffset << 1];
	bx::memSet(dstUV, 0, sizeof(float) * 2 * numVertices); // UVs not used by the shader.
#endif

	const uint32_t color = ColorRGBA::White;
	uint32_t* dstColor = &vb->m_Color[vbOffset];
	memset32(dstColor, numVertices, &color);
#endif

	uint16_t* dstIndex = &m_ActiveIndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

void Context::createDrawCommand_Clip(const float* vtx, uint32_t numVertices, const uint16_t* indices, uint32_t numIndices)
{
	// Allocate the draw command
	DrawCommand* cmd = allocDrawCommand_Clip(numVertices, numIndices);

	// Vertex buffer
	VertexBuffer* vb = &m_VertexBuffers[cmd->m_VertexBufferID];
	const uint32_t vbOffset = cmd->m_FirstVertexID + cmd->m_NumVertices;

	float* dstPos = &vb->m_Pos[vbOffset << 1];
	bx::memCopy(dstPos, vtx, sizeof(float) * 2 * numVertices);

	// Index buffer
	uint16_t* dstIndex = &m_ActiveIndexBuffer->m_Indices[cmd->m_FirstIndexID + cmd->m_NumIndices];
	batchTransformDrawIndices(indices, numIndices, dstIndex, (uint16_t)cmd->m_NumVertices);

	cmd->m_NumVertices += numVertices;
	cmd->m_NumIndices += numIndices;
}

//////////////////////////////////////////////////////////////////////////
// SIMD functions
//
void batchTransformTextQuads(const FONSquad* __restrict quads, uint32_t n, const float* __restrict mtx, float* __restrict transformedVertices)
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
		bx::simd_st(transformedVertices + 4, v23);

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

		bx::simd_st(transformedVertices + 8, v45);
		bx::simd_st(transformedVertices + 12, v67);

		quads += 2;
		transformedVertices += 16;
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
		bx::simd_st(transformedVertices + 4, v23);
	}
#else
	for (uint32_t i = 0; i < n; ++i) {
		const FONSquad* q = &quads[i];
		const uint32_t s = i << 3;
		transformPos2D(q->x0, q->y0, mtx, &transformedVertices[s + 0]);
		transformPos2D(q->x1, q->y0, mtx, &transformedVertices[s + 2]);
		transformPos2D(q->x1, q->y1, mtx, &transformedVertices[s + 4]);
		transformPos2D(q->x0, q->y1, mtx, &transformedVertices[s + 6]);
	}
#endif
}

void batchTransformPositions(const float* __restrict v, uint32_t n, float* __restrict p, const float* __restrict mtx)
{
#if !VG_CONFIG_ENABLE_SIMD
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t id = i << 1;
		transformPos2D(v[id], v[id + 1], mtx, &p[id]);
	}
#else
	const float* src = v;
	float* dst = p;

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

void batchTransformPositions_Unaligned(const float* __restrict v, uint32_t n, float* __restrict p, const float* __restrict mtx)
{
#if !VG_CONFIG_ENABLE_SIMD
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t id = i << 1;
		transformPos2D(v[id], v[id + 1], mtx, &p[id]);
	}
#else
	const float* src = v;
	float* dst = p;

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
	case 7: *dst++ = *src++ + delta;
	case 6: *dst++ = *src++ + delta;
	case 5: *dst++ = *src++ + delta;
	case 4: *dst++ = *src++ + delta;
	case 3: *dst++ = *src++ + delta;
	case 2: *dst++ = *src++ + delta;
	case 1: *dst   = *src   + delta;
	}
#endif
}

#if VG_CONFIG_UV_INT16
void generateUVs(const float* __restrict v, uint32_t n, int16_t* __restrict uv, const float* __restrict mtx)
{
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t id = i << 1;

		float p[2];
		transformPos2D(v[id], v[id + 1], mtx, &p[0]);

		uv[id + 0] = (int16_t)(p[0] * INT16_MAX);
		uv[id + 1] = (int16_t)(p[1] * INT16_MAX);
	}
}
#endif
}
