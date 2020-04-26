#ifndef VG_H
#define VG_H

#include <stdint.h>
#include <bx/math.h>

#ifndef VG_CONFIG_DEBUG
#	define VG_CONFIG_DEBUG 0
#endif

#ifndef VG_CONFIG_ENABLE_SHAPE_CACHING
#	define VG_CONFIG_ENABLE_SHAPE_CACHING 1
#endif

#ifndef VG_CONFIG_ENABLE_SIMD
#	define VG_CONFIG_ENABLE_SIMD 1
#endif

#ifndef VG_CONFIG_FORCE_AA_OFF
#	define VG_CONFIG_FORCE_AA_OFF 0
#endif

#ifndef VG_CONFIG_LIBTESS2_SCRATCH_BUFFER
#	define VG_CONFIG_LIBTESS2_SCRATCH_BUFFER (4 * 1024 * 1024) // Set to 0 to let libtess2 use malloc/free
#endif

#ifndef VG_CONFIG_UV_INT16
#	define VG_CONFIG_UV_INT16 1
#endif

// If set to 1, submitCommandList() calls pustState()/popState() and resetClip() before and after
// executing the commands. Otherwise, the state produced by the command list will affect the global
// state after the execution of the commands.
#ifndef VG_CONFIG_COMMAND_LIST_PRESERVE_STATE
#	define VG_CONFIG_COMMAND_LIST_PRESERVE_STATE 0
#endif

// NOTE: beginCommandList()/endCommandList() blocks require an indirect jump for each function/path command,
// because they change the Context' vtable. If this is set to 0, all functions call their implementation 
// directly (i.e. there will probably still be a jump there but it'll be unconditional/direct).
// If you care about perf so much that an indirect unconditional jump is a problem for you, or if you aren't
// planning on using command lists at all, set this to 0 and use only clXXX functions to build command lists. 
#ifndef VG_CONFIG_COMMAND_LIST_BEGIN_END_API
#	define VG_CONFIG_COMMAND_LIST_BEGIN_END_API 1
#endif

#if VG_CONFIG_DEBUG
#include <bx/debug.h>

#define VG_TRACE(_format, ...) \
	do { \
		bx::debugPrintf(BX_FILE_LINE_LITERAL "vg " _format "\n", ##__VA_ARGS__); \
	} while(0)

#define VG_WARN(_condition, _format, ...) \
	do { \
		if (!(_condition) ) { \
			VG_TRACE(BX_FILE_LINE_LITERAL _format, ##__VA_ARGS__); \
		} \
	} while(0)

#define VG_CHECK(_condition, _format, ...) \
	do { \
		if (!(_condition) ) { \
			VG_TRACE(BX_FILE_LINE_LITERAL _format, ##__VA_ARGS__); \
			bx::debugBreak(); \
		} \
	} while(0)
#else
#define VG_TRACE(_format, ...)
#define VG_WARN(_condition, _format, ...)
#define VG_CHECK(_condition, _format, ...)
#endif

#define VG_HANDLE(_name) struct _name { uint16_t idx; }
#define VG_HANDLE32(_name) struct _name { uint16_t idx; uint16_t flags; }
#define VG_INVALID_HANDLE { UINT16_MAX }
#define VG_INVALID_HANDLE32 { UINT16_MAX, 0 }

#define VG_COLOR_RED_SHIFT   0
#define VG_COLOR_GREEN_SHIFT 8
#define VG_COLOR_BLUE_SHIFT  16
#define VG_COLOR_ALPHA_SHIFT 24
#define VG_COLOR_RGB_MASK    0x00FFFFFF

#define VG_COLOR32(r, g, b, a) (uint32_t)(((uint32_t)(r) << VG_COLOR_RED_SHIFT) | ((uint32_t)(g) << VG_COLOR_GREEN_SHIFT) | ((uint32_t)(b) << VG_COLOR_BLUE_SHIFT) | ((uint32_t)(a) << VG_COLOR_ALPHA_SHIFT))

#define VG_EPSILON 1e-5f

namespace bx
{
struct AllocatorI;
}

namespace bgfx
{
struct TextureHandle;
}

namespace vg
{
typedef uint32_t Color;

Color color4f(float r, float g, float b, float a);
Color color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
Color colorHSB(float h, float s, float b);
Color colorSetAlpha(Color c, uint8_t a);
uint8_t colorGetRed(Color c);
uint8_t colorGetGreen(Color c);
uint8_t colorGetBlue(Color c);
uint8_t colorGetAlpha(Color c);

struct Colors
{
	enum Enum : uint32_t
	{
		Transparent = 0x00000000,
		Black = 0xFF000000,
		Red = 0xFF0000FF,
		Green = 0xFF00FF00,
		Blue = 0xFFFF0000,
		White = 0xFFFFFFFF
	};
};

struct TextAlign
{
	// Values identical to FontStash's alignment flags
	enum Enum : uint32_t
	{
		Left     = 1 << 0,
		Center   = 1 << 1,
		Right    = 1 << 2,
		Top      = 1 << 3,
		Middle   = 1 << 4,
		Bottom   = 1 << 5,
		Baseline = 1 << 6,

		// Shortcuts
		TopLeft        = Top | Left,
		TopCenter      = Top | Center,
		TopRight       = Top | Right,
		MiddleLeft     = Middle | Left,
		MiddleCenter   = Middle | Center,
		MiddleRight    = Middle | Right,
		BottomLeft     = Bottom | Left,
		BottomCenter   = Bottom | Center,
		BottomRight    = Bottom | Right,
		BaselineLeft   = Baseline | Left,
		BaselineCenter = Baseline | Center,
		BaselineRight  = Baseline | Right
	};
};

struct LineCap
{
	enum Enum : uint32_t
	{
		Butt   = 0,
		Round  = 1,
		Square = 2,
	};
};

struct LineJoin
{
	enum Enum : uint32_t
	{
		Miter = 0,
		Round = 1,
		Bevel = 2
	};
};

#define VG_STROKE_FLAGS(cap, join, aa)   (((aa) << 4) | ((cap) << 2) | (join))
#define VG_STROKE_FLAGS_LINE_CAP(flags)  (LineCap::Enum)(((flags) >> 2) & 0x03)
#define VG_STROKE_FLAGS_LINE_JOIN(flags) (LineJoin::Enum)(((flags) >> 0) & 0x03)
#define VG_STROKE_FLAGS_AA(flags)        (((flags) & 0x10) != 0)

struct StrokeFlags
{
	enum Enum : uint32_t
	{
		// w/o AA
		ButtMiter   = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Miter, 0),
		ButtRound   = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Round, 0),
		ButtBevel   = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Bevel, 0),
		RoundMiter  = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Miter, 0),
		RoundRound  = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Round, 0),
		RoundBevel  = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Bevel, 0),
		SquareMiter = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Miter, 0),
		SquareRound = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Round, 0),
		SquareBevel = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Bevel, 0),

		// w/ AA
		ButtMiterAA   = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Miter, 1),
		ButtRoundAA   = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Round, 1),
		ButtBevelAA   = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Bevel, 1),
		RoundMiterAA  = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Miter, 1),
		RoundRoundAA  = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Round, 1),
		RoundBevelAA  = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Bevel, 1),
		SquareMiterAA = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Miter, 1),
		SquareRoundAA = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Round, 1),
		SquareBevelAA = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Bevel, 1),

		FixedWidth = 1u << 5 // NOTE: Scale independent stroke width
	};
};

struct PathType
{
	enum Enum : uint32_t
	{
		Convex  = 0,
		Concave = 1,
	};
};

struct FillRule
{
	enum Enum : uint32_t
	{
		NonZero = 0,
		EvenOdd = 1,
	};
};

#define VG_FILL_FLAGS(type, rule, aa) ((((rule) << 4) | (aa) << 2) | (type))
#define VG_FILL_FLAGS_PATH_TYPE(flags) (PathType::Enum)((flags) & 0x01)
#define VG_FILL_FLAGS_AA(flags) (((flags) & 0x04) != 0)
#define VG_FILL_FLAGS_RULE(flags) (FillRule::Enum)(((flags) & 0x10) >> 4)

struct FillFlags
{
	enum Enum : uint32_t
	{
		Convex  = VG_FILL_FLAGS(PathType::Convex, FillRule::NonZero, 0),
		ConvexAA = VG_FILL_FLAGS(PathType::Convex, FillRule::NonZero, 1),

		ConcaveNonZero = VG_FILL_FLAGS(PathType::Concave, FillRule::NonZero, 0),
		ConcaveEvenOdd = VG_FILL_FLAGS(PathType::Concave, FillRule::EvenOdd, 0),
		ConcaveNonZeroAA = VG_FILL_FLAGS(PathType::Concave, FillRule::NonZero, 1),
		ConcaveEvenOddAA = VG_FILL_FLAGS(PathType::Concave, FillRule::EvenOdd, 1),

		// These are kept for backwards compatibility
		Concave = ConcaveNonZero,
		ConcaveAA = ConcaveNonZeroAA,
	};
};

struct Winding
{
	enum Enum : uint32_t
	{
		CCW = 0,
		CW  = 1,
	};
};

struct TextBoxFlags
{
	enum Enum : uint32_t
	{
		KeepSpaces = 1 << 0
	};
};

struct ImageFlags
{
	enum Enum : uint32_t
	{
		Filter_NearestUV = 1 << 0,
		Filter_NearestW  = 1 << 1,
		Filter_LinearUV  = 1 << 2,
		Filter_LinearW   = 1 << 3,

		// Shortcuts
		Filter_Nearest = Filter_NearestUV | Filter_NearestW,
		Filter_Bilinear = Filter_LinearUV | Filter_NearestW,
		Filter_Trilinear = Filter_LinearUV | Filter_LinearW
	};
};

struct ClipRule
{
	enum Enum : uint32_t
	{
		In  = 0, // fillRule = "nonzero"?
		Out = 1, // fillRule = "evenodd"?
	};
};

struct TransformOrder
{
	enum Enum : uint32_t
	{
		Pre = 0,
		Post = 1,
	};
};

#if VG_CONFIG_UV_INT16
typedef int16_t uv_t;
#else
typedef float uv_t;
#endif

VG_HANDLE32(GradientHandle);
VG_HANDLE32(ImagePatternHandle);
VG_HANDLE(ImageHandle);
VG_HANDLE(FontHandle);
VG_HANDLE(CommandListHandle);

inline bool isValid(GradientHandle _handle)           { return UINT16_MAX != _handle.idx; };
inline bool isValid(ImagePatternHandle _handle)       { return UINT16_MAX != _handle.idx; };
inline bool isValid(ImageHandle _handle)              { return UINT16_MAX != _handle.idx; };
inline bool isValid(FontHandle _handle)               { return UINT16_MAX != _handle.idx; };
inline bool isValid(CommandListHandle _handle)        { return UINT16_MAX != _handle.idx; };

struct ContextConfig
{
	uint16_t m_MaxGradients;        // default: 64
	uint16_t m_MaxImagePatterns;    // default: 64
	uint16_t m_MaxFonts;            // default: 8
	uint16_t m_MaxStateStackSize;   // default: 32
	uint16_t m_MaxImages;           // default: 16
	uint16_t m_MaxCommandLists;     // default: 256
	uint32_t m_MaxVBVertices;       // default: 65536
	uint32_t m_FontAtlasImageFlags; // default: ImageFlags::Filter_Bilinear
	uint32_t m_MaxCommandListDepth; // default: 16
};

struct Stats
{
	uint32_t m_CmdListMemoryTotal;
	uint32_t m_CmdListMemoryUsed;
};

struct TextConfig
{
	FontHandle m_FontHandle;
	float m_FontSize;
	uint32_t m_Alignment;
	Color m_Color;
};

struct Mesh
{
	const float* m_PosBuffer;
	const uint32_t* m_ColorBuffer;
	const uint16_t* m_IndexBuffer;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
};

// NOTE: The following 2 structs are identical to NanoVG because the rest of the code uses
// them a lot. Until I find a reason to replace them, these will do.
struct TextRow
{
	const char* start;	// Pointer to the input text where the row starts.
	const char* end;	// Pointer to the input text where the row ends (one past the last character).
	const char* next;	// Pointer to the beginning of the next row.
	float width;		// Logical width of the row.
	float minx, maxx;	// Actual bounds of the row. Logical with and bounds can differ because of kerning and some parts over extending.
};

struct GlyphPosition
{
	const char* str;	// Position of the glyph in the input string.
	float x;			// The x-coordinate of the logical glyph position.
	float minx, maxx;	// The bounds of the glyph shape.
};

struct CommandListFlags
{
	enum Enum : uint32_t
	{
		Cacheable           = 1 << 0, // Cache the generated geometry in order to avoid retesselation every frame; uses extra memory
		AllowCommandCulling = 1 << 1, // If the scissor rect ends up being zero-sized, don't execute fill/stroke commands.
	};
};

struct FontFlags
{
	enum Enum : uint32_t
	{
		DontCopyData = 1 << 0, // The calling code will keep the font data alive for as long as the Context is alive so there's no need to copy the data internally.
	};
};

struct Context;

// Context
Context* createContext(bx::AllocatorI* allocator, const ContextConfig* cfg = nullptr);
void destroyContext(Context* ctx);

void begin(Context* ctx, uint16_t viewID, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio);
void end(Context* ctx);
void frame(Context* ctx);
const Stats* getStats(Context* ctx);

void beginPath(Context* ctx);
void moveTo(Context* ctx, float x, float y);
void lineTo(Context* ctx, float x, float y);
void cubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
void quadraticTo(Context* ctx, float cx, float cy, float x, float y);
void arcTo(Context* ctx, float x1, float y1, float x2, float y2, float r);
void arc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
void rect(Context* ctx, float x, float y, float w, float h);
void roundedRect(Context* ctx, float x, float y, float w, float h, float r);
void roundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
void circle(Context* ctx, float cx, float cy, float radius);
void ellipse(Context* ctx, float cx, float cy, float rx, float ry);
void polyline(Context* ctx, const float* coords, uint32_t numPoints);
void closePath(Context* ctx);
void fillPath(Context* ctx, Color color, uint32_t flags);
void fillPath(Context* ctx, GradientHandle gradient, uint32_t flags);
void fillPath(Context* ctx, ImagePatternHandle img, Color color, uint32_t flags);
void strokePath(Context* ctx, Color color, float width, uint32_t flags);
void strokePath(Context* ctx, GradientHandle gradient, float width, uint32_t flags);
void strokePath(Context* ctx, ImagePatternHandle img, Color color, float width, uint32_t flags);
void beginClip(Context* ctx, ClipRule::Enum rule);
void endClip(Context* ctx);
void resetClip(Context* ctx);

GradientHandle createLinearGradient(Context* ctx, float sx, float sy, float ex, float ey, Color icol, Color ocol);
GradientHandle createBoxGradient(Context* ctx, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
GradientHandle createRadialGradient(Context* ctx, float cx, float cy, float inr, float outr, Color icol, Color ocol);
ImagePatternHandle createImagePattern(Context* ctx, float cx, float cy, float w, float h, float angle, ImageHandle image);

void setGlobalAlpha(Context* ctx, float alpha);
void pushState(Context* ctx);
void popState(Context* ctx);
void resetScissor(Context* ctx);
void setScissor(Context* ctx, float x, float y, float w, float h);
bool intersectScissor(Context* ctx, float x, float y, float w, float h);
void transformIdentity(Context* ctx);
void transformScale(Context* ctx, float x, float y);
void transformTranslate(Context* ctx, float x, float y);
void transformRotate(Context* ctx, float ang_rad);
void transformMult(Context* ctx, const float* mtx, TransformOrder::Enum order);
void setViewBox(Context* ctx, float x, float y, float w, float h);

void getTransform(Context* ctx, float* mtx);
void getScissor(Context* ctx, float* rect);

FontHandle createFont(Context* ctx, const char* name, uint8_t* data, uint32_t size, uint32_t flags);
FontHandle getFontByName(Context* ctx, const char* name);
bool setFallbackFont(Context* ctx, FontHandle base, FontHandle fallback);
void text(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end);
void textBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* text, const char* end, uint32_t textboxFlags);
float measureText(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end, float* bounds);
void measureTextBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags);
float getTextLineHeight(Context* ctx, const TextConfig& cfg);
int textBreakLines(Context* ctx, const TextConfig& cfg, const char* str, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags);
int textGlyphPositions(Context* ctx, const TextConfig& cfg, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions);

/*
 * pos: A list of 2D vertices (successive x,y pairs)
 * uv (optional): 1 UV pair for each position. If not specified (nullptr), the white-rect UV from the font atlas is used.
 * numVertices: the number of vertices (if uv != nullptr, the number of uv pairs is supposed to be equal to this)
 * color: Either a single color (which is replicated on all vertices) or a list of colors, 1 for each vertex.
 * numColors: The number of colors in the color array (can be either 1 for solid color fills or equal to numVertices for per-vertex gradients).
 * indices: A list of indices (triangle list)
 * numIndices: The number of indices
 * img (optional): The image to use for this draw call (created via createImage()) or VG_INVALID_HANDLE in case you don't have an image (colored tri-list).
 */
void indexedTriList(Context* ctx, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img);

bool getImageSize(Context* ctx, ImageHandle handle, uint16_t* w, uint16_t* h);
ImageHandle createImage(Context* ctx, uint16_t w, uint16_t h, uint32_t flags, const uint8_t* data);
ImageHandle createImage(Context* ctx, uint32_t flags, const bgfx::TextureHandle& bgfxTextureHandle);
bool updateImage(Context* ctx, ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
bool destroyImage(Context* ctx, ImageHandle img);
bool isImageValid(Context* ctx, ImageHandle img);

// Command lists
CommandListHandle createCommandList(Context* ctx, uint32_t flags);
void destroyCommandList(Context* ctx, CommandListHandle handle);
void resetCommandList(Context* ctx, CommandListHandle handle);
void submitCommandList(Context* ctx, CommandListHandle handle);
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
void beginCommandList(Context* ctx, CommandListHandle handle);
void endCommandList(Context* ctx);
#endif

void clBeginPath(Context* ctx, CommandListHandle handle);
void clMoveTo(Context* ctx, CommandListHandle handle, float x, float y);
void clLineTo(Context* ctx, CommandListHandle handle, float x, float y);
void clCubicTo(Context* ctx, CommandListHandle handle, float c1x, float c1y, float c2x, float c2y, float x, float y);
void clQuadraticTo(Context* ctx, CommandListHandle handle, float cx, float cy, float x, float y);
void clArcTo(Context* ctx, CommandListHandle handle, float x1, float y1, float x2, float y2, float r);
void clArc(Context* ctx, CommandListHandle handle, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
void clRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);
void clRoundedRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r);
void clRoundedRectVarying(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
void clCircle(Context* ctx, CommandListHandle handle, float cx, float cy, float radius);
void clEllipse(Context* ctx, CommandListHandle handle, float cx, float cy, float rx, float ry);
void clPolyline(Context* ctx, CommandListHandle handle, const float* coords, uint32_t numPoints);
void clClosePath(Context* ctx, CommandListHandle handle);
void clIndexedTriList(Context* ctx, CommandListHandle handle, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img);
void clFillPath(Context* ctx, CommandListHandle handle, Color color, uint32_t flags);
void clFillPath(Context* ctx, CommandListHandle handle, GradientHandle gradient, uint32_t flags);
void clFillPath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, Color color, float width, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, GradientHandle gradient, float width, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, float width, uint32_t flags);
void clBeginClip(Context* ctx, CommandListHandle handle, ClipRule::Enum rule);
void clEndClip(Context* ctx, CommandListHandle handle);
void clResetClip(Context* ctx, CommandListHandle handle);

GradientHandle clCreateLinearGradient(Context* ctx, CommandListHandle handle, float sx, float sy, float ex, float ey, Color icol, Color ocol);
GradientHandle clCreateBoxGradient(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
GradientHandle clCreateRadialGradient(Context* ctx, CommandListHandle handle, float cx, float cy, float inr, float outr, Color icol, Color ocol);
ImagePatternHandle clCreateImagePattern(Context* ctx, CommandListHandle handle, float cx, float cy, float w, float h, float angle, ImageHandle image);

void clPushState(Context* ctx, CommandListHandle handle);
void clPopState(Context* ctx, CommandListHandle handle);
void clResetScissor(Context* ctx, CommandListHandle handle);
void clSetScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);
void clIntersectScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);
void clTransformIdentity(Context* ctx, CommandListHandle handle);
void clTransformScale(Context* ctx, CommandListHandle handle, float x, float y);
void clTransformTranslate(Context* ctx, CommandListHandle handle, float x, float y);
void clTransformRotate(Context* ctx, CommandListHandle handle, float ang_rad);
void clTransformMult(Context* ctx, CommandListHandle handle, const float* mtx, TransformOrder::Enum order);
void clSetViewBox(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);

void clText(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, const char* str, const char* end);
void clTextBox(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);

void clSubmitCommandList(Context* ctx, CommandListHandle parent, CommandListHandle child);

//////////////////////////////////////////////////////////////////////////
// Helpers
//
TextConfig makeTextConfig(Context* ctx, const char* fontName, float fontSize, uint32_t alignment, Color color);
TextConfig makeTextConfig(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color);
void text(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color, float x, float y, const char* str, const char* end);
void textBox(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);
float measureText(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, const char* str, const char* end, float* bounds);
void measureTextBox(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, float breakWidth, const char* str, const char* end, float* bounds, uint32_t textboxFlags);
float getTextLineHeight(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment);
int textBreakLines(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, const char* str, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags);
int textGlyphPositions(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, const char* str, const char* end, GlyphPosition* positions, int maxPositions);

// Helper struct and functions to avoid moving around both a Context and a CommandListHandle.
struct CommandListRef
{
	Context* m_Context;
	CommandListHandle m_Handle;
};

CommandListRef makeCommandListRef(Context* ctx, CommandListHandle handle);
void clReset(CommandListRef& ref);
void clBeginPath(CommandListRef& ref);
void clMoveTo(CommandListRef& ref, float x, float y);
void clLineTo(CommandListRef& ref, float x, float y);
void clCubicTo(CommandListRef& ref, float c1x, float c1y, float c2x, float c2y, float x, float y);
void clQuadraticTo(CommandListRef& ref, float cx, float cy, float x, float y);
void clArc(CommandListRef& ref, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
void clArcTo(CommandListRef& ref, float x1, float y1, float x2, float y2, float r);
void clRect(CommandListRef& ref, float x, float y, float w, float h);
void clRoundedRect(CommandListRef& ref, float x, float y, float w, float h, float r);
void clRoundedRectVarying(CommandListRef& ref, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
void clCircle(CommandListRef& ref, float cx, float cy, float radius);
void clEllipse(CommandListRef& ref, float cx, float cy, float rx, float ry);
void clPolyline(CommandListRef& ref, const float* coords, uint32_t numPoints);
void clClosePath(CommandListRef& ref);
void clFillPath(CommandListRef& ref, Color color, uint32_t flags);
void clFillPath(CommandListRef& ref, GradientHandle gradient, uint32_t flags);
void clFillPath(CommandListRef& ref, ImagePatternHandle img, Color color, uint32_t flags);
void clStrokePath(CommandListRef& ref, Color color, float width, uint32_t flags);
void clStrokePath(CommandListRef& ref, GradientHandle gradient, float width, uint32_t flags);
void clStrokePath(CommandListRef& ref, ImagePatternHandle img, Color color, float width, uint32_t flags);
void clBeginClip(CommandListRef& ref, ClipRule::Enum rule);
void clEndClip(CommandListRef& ref);
void clResetClip(CommandListRef& ref);
GradientHandle clCreateLinearGradient(CommandListRef& ref, float sx, float sy, float ex, float ey, Color icol, Color ocol);
GradientHandle clCreateBoxGradient(CommandListRef& ref, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
GradientHandle clCreateRadialGradient(CommandListRef& ref, float cx, float cy, float inr, float outr, Color icol, Color ocol);
ImagePatternHandle clCreateImagePattern(CommandListRef& ref, float cx, float cy, float w, float h, float angle, ImageHandle image);
void clPushState(CommandListRef& ref);
void clPopState(CommandListRef& ref);
void clResetScissor(CommandListRef& ref);
void clSetScissor(CommandListRef& ref, float x, float y, float w, float h);
void clIntersectScissor(CommandListRef& ref, float x, float y, float w, float h);
void clTransformIdentity(CommandListRef& ref);
void clTransformScale(CommandListRef& ref, float x, float y);
void clTransformTranslate(CommandListRef& ref, float x, float y);
void clTransformRotate(CommandListRef& ref, float ang_rad);
void clTransformMult(CommandListRef& ref, const float* mtx, TransformOrder::Enum order);
void clText(CommandListRef& ref, const TextConfig& cfg, float x, float y, const char* str, const char* end);
void clTextBox(CommandListRef& ref, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);
void clSubmitCommandList(CommandListRef& ref, CommandListHandle child);
}

#include "inline/vg.inl"

#endif // VG_H
