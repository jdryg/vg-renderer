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

// TODO: Doesn't work without libtess2!
#ifndef VG_CONFIG_USE_LIBTESS2
#	define VG_CONFIG_USE_LIBTESS2 1
#endif

#ifndef VG_CONFIG_LIBTESS2_SCRATCH_BUFFER
#	define VG_CONFIG_LIBTESS2_SCRATCH_BUFFER (4 * 1024 * 1024) // Set to 0 to let libtess2 use malloc/free
#endif

#ifndef VG_CONFIG_UV_INT16
#	define VG_CONFIG_UV_INT16 1
#endif

#if VG_CONFIG_DEBUG
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
#define VG_INVALID_HANDLE { UINT16_MAX }

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

namespace vg
{
typedef uint32_t Color;

struct ColorRGBA
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

	static Color fromFloat(float r, float g, float b, float a)
	{
		uint8_t rb = (uint8_t)(r * 255.0f);
		uint8_t gb = (uint8_t)(g * 255.0f);
		uint8_t bb = (uint8_t)(b * 255.0f);
		uint8_t ab = (uint8_t)(a * 255.0f);
		return VG_COLOR32(rb, gb, bb, ab);
	}

	static Color fromByte(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		return VG_COLOR32(r, g, b, a);
	}

	static Color setAlpha(Color c, uint8_t a)
	{
		return (c & VG_COLOR_RGB_MASK) | ((uint32_t)a << VG_COLOR_ALPHA_SHIFT);
	}

	static Color fromHSB(float hue, float sat, float brightness)
	{
		const float d = 1.0f / 6.0f;

		const float C = brightness * sat;
		const float X = C * (1.0f - bx::abs(bx::mod((hue * 6.0f), 2.0f) - 1.0f));
		const float m = brightness - C;

		float fred = 0.0f;
		float fgreen = 0.0f;
		float fblue = 0.0f;
		if (hue <= d) {
			fred = C;
			fgreen = X;
		} else if (hue > d && hue <= 2.0f * d) {
			fred = X;
			fgreen = C;
		} else if (hue > 2.0f * d && hue <= 3.0f * d) {
			fgreen = C;
			fblue = X;
		} else if (hue > 3.0f * d && hue <= 4.0f * d) {
			fgreen = X;
			fblue = C;
		} else if (hue > 4.0f * d && hue <= 5.0f * d) {
			fred = X;
			fblue = C;
		} else {
			fred = C;
			fblue = X;
		}

		uint32_t r = (uint32_t)bx::floor((fred + m) * 255.0f);
		uint32_t g = (uint32_t)bx::floor((fgreen + m) * 255.0f);
		uint32_t b = (uint32_t)bx::floor((fblue + m) * 255.0f);

		return 0xFF000000 | (b << 16) | (g << 8) | (r);
	}

	static uint8_t getAlpha(Color c) { return (uint8_t)((c >> VG_COLOR_ALPHA_SHIFT) & 0xFF); }
	static uint8_t getRed(Color c) { return (uint8_t)((c >> VG_COLOR_RED_SHIFT) & 0xFF); }
	static uint8_t getGreen(Color c) { return (uint8_t)((c >> VG_COLOR_GREEN_SHIFT) & 0xFF); }
	static uint8_t getBlue(Color c) { return (uint8_t)((c >> VG_COLOR_BLUE_SHIFT) & 0xFF); }
};

struct TextAlign
{
	// Values identical to FontStash's alignment flags
	enum Enum : uint32_t
	{
		Left = 1 << 0,
		Center = 1 << 1,
		Right = 1 << 2,
		Top = 1 << 3,
		Middle = 1 << 4,
		Bottom = 1 << 5,
		Baseline = 1 << 6,

		// Shortcuts
		TopLeft = Top | Left,
		MiddleCenter = Middle | Center,
		BottomCenter = Bottom | Center
	};
};

struct LineCap
{
	enum Enum : uint32_t
	{
		Butt = 0,
		Round = 1,
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

#define VG_STROKE_FLAGS(cap, join, aa) (((aa) << 4) | ((cap) << 2) | (join))
#define VG_STROKE_FLAGS_LINE_CAP(flags) (LineCap::Enum)(((flags) >> 2) & 0x03)
#define VG_STROKE_FLAGS_LINE_JOIN(flags) (LineJoin::Enum)(((flags) >> 0) & 0x03)
#define VG_STROKE_FLAGS_AA(flags) (((flags) & 0x10) != 0)

struct StrokeFlags
{
	enum Enum : uint32_t
	{
		// w/o AA
		ButtMiter = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Miter, 0),
		ButtRound = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Round, 0),
		ButtBevel = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Bevel, 0),
		RoundMiter = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Miter, 0),
		RoundRound = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Round, 0),
		RoundBevel = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Bevel, 0),
		SquareMiter = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Miter, 0),
		SquareRound = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Round, 0),
		SquareBevel = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Bevel, 0),

		// w/ AA
		ButtMiterAA = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Miter, 1),
		ButtRoundAA = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Round, 1),
		ButtBevelAA = VG_STROKE_FLAGS(LineCap::Butt, LineJoin::Bevel, 1),
		RoundMiterAA = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Miter, 1),
		RoundRoundAA = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Round, 1),
		RoundBevelAA = VG_STROKE_FLAGS(LineCap::Round, LineJoin::Bevel, 1),
		SquareMiterAA = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Miter, 1),
		SquareRoundAA = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Round, 1),
		SquareBevelAA = VG_STROKE_FLAGS(LineCap::Square, LineJoin::Bevel, 1),
	};
};

struct PathType
{
	enum Enum : uint32_t
	{
		Convex = 0,
		Concave = 1,
	};
};

#define VG_FILL_FLAGS(type, aa) (((aa) << 4) | (type))
#define VG_FILL_FLAGS_PATH_TYPE(flags) (PathType::Enum)((flags) & 0x01)
#define VG_FILL_FLAGS_AA(flags) (((flags) & 0x10) != 0)

struct FillFlags
{
	enum Enum : uint32_t
	{
		// w/o AA
		Convex = VG_FILL_FLAGS(PathType::Convex, 0),
		Concave = VG_FILL_FLAGS(PathType::Concave, 0),

		// w/ AA
		ConvexAA = VG_FILL_FLAGS(PathType::Convex, 1),
		ConcaveAA = VG_FILL_FLAGS(PathType::Concave, 1),
	};
};

struct Winding
{
	enum Enum : uint32_t
	{
		CCW = 0,
		CW = 1,
	};
};

struct TextBreakFlags
{
	enum Enum : uint32_t
	{
		SpacesAsChars = 0x00000001
	};
};

struct ImageFlags
{
	enum Enum : uint32_t
	{
		Filter_NearestUV = 0x00000001,
		Filter_NearestW = 0x00000002,
		Filter_LinearUV = 0x00000004,
		Filter_LinearW = 0x00000008,

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
		In = 0,  // fillRule = "nonzero"?
		Out = 1, // fillRule = "evenodd"?
	};
};

#if VG_CONFIG_UV_INT16
typedef int16_t uv_t;
#else
typedef float uv_t;
#endif

VG_HANDLE(GradientHandle);
VG_HANDLE(ImagePatternHandle);
VG_HANDLE(ImageHandle);
VG_HANDLE(FontHandle);
VG_HANDLE(CommandListHandle);

// Handles generated by command lists (clCreateXXXGradient() and clCreateImagePattern());
VG_HANDLE(LocalGradientHandle);
VG_HANDLE(LocalImagePatternHandle);

inline bool isValid(GradientHandle _handle)           { return UINT16_MAX != _handle.idx; };
inline bool isValid(ImagePatternHandle _handle)       { return UINT16_MAX != _handle.idx; };
inline bool isValid(ImageHandle _handle)              { return UINT16_MAX != _handle.idx; };
inline bool isValid(FontHandle _handle)               { return UINT16_MAX != _handle.idx; };
inline bool isValid(CommandListHandle _handle)        { return UINT16_MAX != _handle.idx; };
inline bool isValid(LocalGradientHandle _handle)      { return UINT16_MAX != _handle.idx; };
inline bool isValid(LocalImagePatternHandle _handle)  { return UINT16_MAX != _handle.idx; };

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
		Cacheable = 0x00000001,           // Cache the generated geometry in order to avoid retesselation every frame; uses extra memory
		AllowCommandCulling = 0x00000002, // If the scissor rect ends up being zero-sized, don't execute fill/stroke commands.
	};
};

struct FontFlags
{
	enum Enum : uint32_t
	{
		DontCopyData = 0x00000001, // The calling code will keep the font data alive for as long as the Context is alive so there's no need to copy the data internally.
	};
};

struct Context;

// Context
Context* createContext(uint16_t viewID, bx::AllocatorI* allocator, const ContextConfig* cfg = nullptr);
void destroyContext(Context* ctx);

void beginFrame(Context* ctx, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio);
void endFrame(Context* ctx);
void beginPath(Context* ctx);
void moveTo(Context* ctx, float x, float y);
void lineTo(Context* ctx, float x, float y);
void cubicTo(Context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
void quadraticTo(Context* ctx, float cx, float cy, float x, float y);
void arcTo(Context* ctx, float x1, float y1, float x2, float y2, float r);
void arc(Context* ctx, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
void rect(Context* ctx, float x, float y, float w, float h);
void roundedRect(Context* ctx, float x, float y, float w, float h, float r);
void roundedRectVarying(Context* ctx, float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
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
void transformMult(Context* ctx, const float* mtx, bool pre);

FontHandle createFont(Context* ctx, const char* name, uint8_t* data, uint32_t size, uint32_t flags);
FontHandle getFontByName(Context* ctx, const char* name);
bool setFallbackFont(Context* ctx, FontHandle base, FontHandle fallback);
void text(Context* ctx, const TextConfig& cfg, float x, float y, const char* str, const char* end);
void textBox(Context* ctx, const TextConfig& cfg, float x, float y, float breakWidth, const char* text, const char* end);
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
bool updateImage(Context* ctx, ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
bool deleteImage(Context* ctx, ImageHandle img);
bool isImageValid(Context* ctx, ImageHandle img);

// Command lists
CommandListHandle createCommandList(Context* ctx, uint32_t flags);
void destroyCommandList(Context* ctx, CommandListHandle handle);
void submitCommandList(Context* ctx, CommandListHandle handle);

void clReset(Context* ctx, CommandListHandle handle);
void clBeginPath(Context* ctx, CommandListHandle handle);
void clMoveTo(Context* ctx, CommandListHandle handle, float x, float y);
void clLineTo(Context* ctx, CommandListHandle handle, float x, float y);
void clCubicTo(Context* ctx, CommandListHandle handle, float c1x, float c1y, float c2x, float c2y, float x, float y);
void clQuadraticTo(Context* ctx, CommandListHandle handle, float cx, float cy, float x, float y);
void clArcTo(Context* ctx, CommandListHandle handle, float x1, float y1, float x2, float y2, float r);
void clArc(Context* ctx, CommandListHandle handle, float cx, float cy, float r, float a0, float a1, Winding::Enum dir);
void clRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);
void clRoundedRect(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r);
void clRoundedRectVarying(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
void clCircle(Context* ctx, CommandListHandle handle, float cx, float cy, float radius);
void clEllipse(Context* ctx, CommandListHandle handle, float cx, float cy, float rx, float ry);
void clPolyline(Context* ctx, CommandListHandle handle, const float* coords, uint32_t numPoints);
void clClosePath(Context* ctx, CommandListHandle handle);
void clIndexedTriList(Context* ctx, CommandListHandle handle, const float* pos, const uv_t* uv, uint32_t numVertices, const Color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, ImageHandle img);
void clFillPath(Context* ctx, CommandListHandle handle, Color color, uint32_t flags);
void clFillPath(Context* ctx, CommandListHandle handle, GradientHandle gradient, uint32_t flags);
void clFillPath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, uint32_t flags);
void clFillPath(Context* ctx, CommandListHandle handle, LocalGradientHandle gradient, uint32_t flags);
void clFillPath(Context* ctx, CommandListHandle handle, LocalImagePatternHandle img, Color color, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, Color color, float width, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, GradientHandle gradient, float width, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, ImagePatternHandle img, Color color, float width, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, LocalGradientHandle gradient, float width, uint32_t flags);
void clStrokePath(Context* ctx, CommandListHandle handle, LocalImagePatternHandle img, Color color, float width, uint32_t flags);
void clBeginClip(Context* ctx, CommandListHandle handle, ClipRule::Enum rule);
void clEndClip(Context* ctx, CommandListHandle handle);
void clResetClip(Context* ctx, CommandListHandle handle);

LocalGradientHandle clCreateLinearGradient(Context* ctx, CommandListHandle handle, float sx, float sy, float ex, float ey, Color icol, Color ocol);
LocalGradientHandle clCreateBoxGradient(Context* ctx, CommandListHandle handle, float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
LocalGradientHandle clCreateRadialGradient(Context* ctx, CommandListHandle handle, float cx, float cy, float inr, float outr, Color icol, Color ocol);
LocalImagePatternHandle clCreateImagePattern(Context* ctx, CommandListHandle handle, float cx, float cy, float w, float h, float angle, ImageHandle image);

void clPushState(Context* ctx, CommandListHandle handle);
void clPopState(Context* ctx, CommandListHandle handle);
void clResetScissor(Context* ctx, CommandListHandle handle);
void clSetScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);
void clIntersectScissor(Context* ctx, CommandListHandle handle, float x, float y, float w, float h);
void clTransformIdentity(Context* ctx, CommandListHandle handle);
void clTransformScale(Context* ctx, CommandListHandle handle, float x, float y);
void clTransformTranslate(Context* ctx, CommandListHandle handle, float x, float y);
void clTransformRotate(Context* ctx, CommandListHandle handle, float ang_rad);
void clTransformMult(Context* ctx, CommandListHandle handle, const float* mtx, bool pre);

void clText(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, const char* str, const char* end);
void clTextBox(Context* ctx, CommandListHandle handle, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end);

//////////////////////////////////////////////////////////////////////////
// Helpers
//
inline TextConfig makeTextConfig(Context* ctx, const char* fontName, float fontSize, uint32_t alignment, Color color)
{
	return { getFontByName(ctx, fontName), fontSize, alignment, color };
}

inline TextConfig makeTextConfig(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color)
{
	BX_UNUSED(ctx);
	return { fontHandle, fontSize, alignment, color };
}

inline void text(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color, float x, float y, const char* str, const char* end)
{
	text(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, color), x, y, str, end);
}

inline void textBox(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* str, const char* end)
{
	textBox(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, color), x, y, breakWidth, str, end);
}

inline float measureText(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, const char* str, const char* end, float* bounds)
{
	return measureText(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, ColorRGBA::Transparent), x, y, str, end, bounds);
}

inline void measureTextBox(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, float breakWidth, const char* str, const char* end, float* bounds, uint32_t flags)
{
	measureTextBox(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, ColorRGBA::Transparent), x, y, breakWidth, str, end, bounds, flags);
}

inline float getTextLineHeight(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment)
{
	return getTextLineHeight(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, ColorRGBA::Transparent));
}

inline int textBreakLines(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, const char* str, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	return textBreakLines(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, ColorRGBA::Transparent), str, end, breakRowWidth, rows, maxRows, flags);
}

inline int textGlyphPositions(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, const char* str, const char* end, GlyphPosition* positions, int maxPositions)
{
	return textGlyphPositions(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, ColorRGBA::Transparent), x, y, str, end, positions, maxPositions);
}

// Helper struct and functions to avoid moving around both a context and a command list handle.
struct CommandListRef
{
	Context* m_Context;
	CommandListHandle m_Handle;
};

inline CommandListRef createCommandListRef(Context* ctx, CommandListHandle handle)
{
	return { ctx, handle };
}

inline void clReset(CommandListRef& ref)
{
	clReset(ref.m_Context, ref.m_Handle);
}

inline void clBeginPath(CommandListRef& ref)
{
	clBeginPath(ref.m_Context, ref.m_Handle);
}

inline void clMoveTo(CommandListRef& ref, float x, float y)
{
	clMoveTo(ref.m_Context, ref.m_Handle, x, y);
}

inline void clLineTo(CommandListRef& ref, float x, float y)
{
	clLineTo(ref.m_Context, ref.m_Handle, x, y);
}

inline void clCubicTo(CommandListRef& ref, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	clCubicTo(ref.m_Context, ref.m_Handle, c1x, c1y, c2x, c2y, x, y);
}

inline void clQuadraticTo(CommandListRef& ref, float cx, float cy, float x, float y)
{
	clQuadraticTo(ref.m_Context, ref.m_Handle, cx, cy, x, y);
}

inline void clArc(CommandListRef& ref, float cx, float cy, float r, float a0, float a1, Winding::Enum dir)
{
	clArc(ref.m_Context, ref.m_Handle, cx, cy, r, a0, a1, dir);
}

inline void clArcTo(CommandListRef& ref, float x1, float y1, float x2, float y2, float r)
{
	clArcTo(ref.m_Context, ref.m_Handle, x1, y1, x2, y2, r);
}

inline void clRect(CommandListRef& ref, float x, float y, float w, float h)
{
	clRect(ref.m_Context, ref.m_Handle, x, y, w, h);
}

inline void clRoundedRect(CommandListRef& ref, float x, float y, float w, float h, float r)
{
	clRoundedRect(ref.m_Context, ref.m_Handle, x, y, w, h, r);
}

inline void clRoundedRectVarying(CommandListRef& ref, float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	clRoundedRectVarying(ref.m_Context, ref.m_Handle, x, y, w, h, rtl, rbl, rbr, rtr);
}

inline void clCircle(CommandListRef& ref, float cx, float cy, float radius)
{
	clCircle(ref.m_Context, ref.m_Handle, cx, cy, radius);
}

inline void clEllipse(CommandListRef& ref, float cx, float cy, float rx, float ry)
{
	clEllipse(ref.m_Context, ref.m_Handle, cx, cy, rx, ry);
}

inline void clPolyline(CommandListRef& ref, const float* coords, uint32_t numPoints)
{
	clPolyline(ref.m_Context, ref.m_Handle, coords, numPoints);
}

inline void clClosePath(CommandListRef& ref)
{
	clClosePath(ref.m_Context, ref.m_Handle);
}

inline void clFillPath(CommandListRef& ref, Color color, uint32_t flags)
{
	clFillPath(ref.m_Context, ref.m_Handle, color, flags);
}

inline void clFillPath(CommandListRef& ref, GradientHandle gradient, uint32_t flags)
{
	clFillPath(ref.m_Context, ref.m_Handle, gradient, flags);
}

inline void clFillPath(CommandListRef& ref, ImagePatternHandle img, Color color, uint32_t flags)
{
	clFillPath(ref.m_Context, ref.m_Handle, img, color, flags);
}

inline void clFillPath(CommandListRef& ref, LocalGradientHandle gradient, uint32_t flags)
{
	clFillPath(ref.m_Context, ref.m_Handle, gradient, flags);
}

inline void clFillPath(CommandListRef& ref, LocalImagePatternHandle img, Color color, uint32_t flags)
{
	clFillPath(ref.m_Context, ref.m_Handle, img, color, flags);
}

inline void clStrokePath(CommandListRef& ref, Color color, float width, uint32_t flags)
{
	clStrokePath(ref.m_Context, ref.m_Handle, color, width, flags);
}

inline void clStrokePath(CommandListRef& ref, GradientHandle gradient, float width, uint32_t flags)
{
	clStrokePath(ref.m_Context, ref.m_Handle, gradient, width, flags);
}

inline void clStrokePath(CommandListRef& ref, ImagePatternHandle img, Color color, float width, uint32_t flags)
{
	clStrokePath(ref.m_Context, ref.m_Handle, img, color, width, flags);
}

inline void clStrokePath(CommandListRef& ref, LocalGradientHandle gradient, float width, uint32_t flags)
{
	clStrokePath(ref.m_Context, ref.m_Handle, gradient, width, flags);
}

inline void clStrokePath(CommandListRef& ref, LocalImagePatternHandle img, Color color, float width, uint32_t flags)
{
	clStrokePath(ref.m_Context, ref.m_Handle, img, color, width, flags);
}

inline void clBeginClip(CommandListRef& ref, ClipRule::Enum rule)
{
	clBeginClip(ref.m_Context, ref.m_Handle, rule);
}

inline void clEndClip(CommandListRef& ref)
{
	clEndClip(ref.m_Context, ref.m_Handle);
}

inline void clResetClip(CommandListRef& ref)
{
	clResetClip(ref.m_Context, ref.m_Handle);
}

inline LocalGradientHandle clCreateLinearGradient(CommandListRef& ref, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	return clCreateLinearGradient(ref.m_Context, ref.m_Handle, sx, sy, ex, ey, icol, ocol);
}

inline LocalGradientHandle clCreateBoxGradient(CommandListRef& ref, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	return clCreateBoxGradient(ref.m_Context, ref.m_Handle, x, y, w, h, r, f, icol, ocol);
}

inline LocalGradientHandle clCreateRadialGradient(CommandListRef& ref, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	return clCreateRadialGradient(ref.m_Context, ref.m_Handle, cx, cy, inr, outr, icol, ocol);
}

inline LocalImagePatternHandle clCreateImagePattern(CommandListRef& ref, float cx, float cy, float w, float h, float angle, ImageHandle image)
{
	return clCreateImagePattern(ref.m_Context, ref.m_Handle, cx, cy, w, h, angle, image);
}

inline void clPushState(CommandListRef& ref)
{
	clPushState(ref.m_Context, ref.m_Handle);
}

inline void clPopState(CommandListRef& ref)
{
	clPopState(ref.m_Context, ref.m_Handle);
}

inline void clResetScissor(CommandListRef& ref)
{
	clResetScissor(ref.m_Context, ref.m_Handle);
}

inline void clSetScissor(CommandListRef& ref, float x, float y, float w, float h)
{
	clSetScissor(ref.m_Context, ref.m_Handle, x, y, w, h);
}

inline void clIntersectScissor(CommandListRef& ref, float x, float y, float w, float h)
{
	clIntersectScissor(ref.m_Context, ref.m_Handle, x, y, w, h);
}

inline void clTransformIdentity(CommandListRef& ref)
{
	clTransformIdentity(ref.m_Context, ref.m_Handle);
}

inline void clTransformScale(CommandListRef& ref, float x, float y)
{
	clTransformScale(ref.m_Context, ref.m_Handle, x, y);
}

inline void clTransformTranslate(CommandListRef& ref, float x, float y)
{
	clTransformTranslate(ref.m_Context, ref.m_Handle, x, y);
}

inline void clTransformRotate(CommandListRef& ref, float ang_rad)
{
	clTransformRotate(ref.m_Context, ref.m_Handle, ang_rad);
}

inline void clTransformMult(CommandListRef& ref, const float* mtx, bool pre)
{
	clTransformMult(ref.m_Context, ref.m_Handle, mtx, pre);
}

inline void clText(CommandListRef& ref, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
	clText(ref.m_Context, ref.m_Handle, cfg, x, y, str, end);
}

inline void clTextBox(CommandListRef& ref, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end)
{
	clTextBox(ref.m_Context, ref.m_Handle, cfg, x, y, breakWidth, str, end);
}
}

#endif
