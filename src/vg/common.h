#ifndef VG_COMMON_H
#define VG_COMMON_H

#ifndef VG_CONFIG_DEBUG
#	define VG_CONFIG_DEBUG 0
#endif

#ifndef VG_CONFIG_SHAPE_DYNAMIC_TEXT
#	define VG_CONFIG_SHAPE_DYNAMIC_TEXT 1
#endif

#ifndef VG_CONFIG_MAX_GRADIENTS
#	define VG_CONFIG_MAX_GRADIENTS 64
#endif

#ifndef VG_CONFIG_MAX_IMAGE_PATTERNS
#	define VG_CONFIG_MAX_IMAGE_PATTERNS 64
#endif

#ifndef VG_CONFIG_MAX_FONTS
#	define VG_CONFIG_MAX_FONTS 8
#endif

#ifndef VG_CONFIG_ENABLE_SHAPE_CACHING
#	define VG_CONFIG_ENABLE_SHAPE_CACHING 1
#endif

#ifndef VG_CONFIG_ENABLE_SIMD
#	define VG_CONFIG_ENABLE_SIMD 1
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

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
#include <functional>
namespace vg
{
// stringID: The same value passed to Shape::TextDynamic
// len: Should be filled with the length of the string or ~0u if the string is null terminated and you don't want to strlen() it.
// userData: The same pointer passed to IRenderer::SubmitShape()
// 
// NOTE: This can be changed to (e.g.) a function pointer, as long as the signature remains the same.
typedef std::function<const char* (uint32_t /*stringID*/, uint32_t& /*len*/, void* /*userData*/)> GetStringByIDFunc;
}
#endif // VG_CONFIG_SHAPE_DYNAMIC_TEXT

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

VG_HANDLE(GradientHandle);
VG_HANDLE(ImagePatternHandle);
VG_HANDLE(ImageHandle);
VG_HANDLE(FontHandle);

inline bool isValid(GradientHandle _handle) { return UINT16_MAX != _handle.idx; };
inline bool isValid(ImagePatternHandle _handle) { return UINT16_MAX != _handle.idx; };
inline bool isValid(ImageHandle _handle) { return UINT16_MAX != _handle.idx; };
inline bool isValid(FontHandle _handle) { return UINT16_MAX != _handle.idx; };

struct Font
{
	FontHandle m_Handle;
	float m_Size;
};

struct String
{
	char* m_Text;
	void* m_RendererData;
	Font m_Font;
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
}

#endif
