#ifndef VG_IRENDERER_H
#define VG_IRENDERER_H

#include <stdint.h>
#include <bgfx/bgfx.h> // BGFX_HANDLE

#define COLOR_RED_SHIFT   0
#define COLOR_GREEN_SHIFT 8
#define COLOR_BLUE_SHIFT  16
#define COLOR_ALPHA_SHIFT 24
#define COLOR_RGB_MASK    0x00FFFFFF

#define COLOR32(r, g, b, a) (uint32_t)(((uint32_t)(r) << COLOR_RED_SHIFT) | ((uint32_t)(g) << COLOR_GREEN_SHIFT) | ((uint32_t)(b) << COLOR_BLUE_SHIFT) | ((uint32_t)(a) << COLOR_ALPHA_SHIFT))

namespace vg
{
struct Shape;

typedef uint32_t Color;

struct ColorRGBA
{
	enum Enum : uint32_t
	{
		Transparent = 0x00000000,
		Black       = 0xFF000000,
		Red         = 0xFF0000FF,
		Green       = 0xFF00FF00,
		White       = 0xFFFFFFFF
	};

	static Color fromFloat(float r, float g, float b, float a)
	{
		uint8_t rb = (uint8_t)(r * 255.0f);
		uint8_t gb = (uint8_t)(g * 255.0f);
		uint8_t bb = (uint8_t)(b * 255.0f);
		uint8_t ab = (uint8_t)(a * 255.0f);
		return COLOR32(rb, gb, bb, ab);
	}

	static Color fromByte(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		return COLOR32(r, g, b, a);
	}

	static Color setAlpha(Color c, uint8_t a)
	{
		return (c & COLOR_RGB_MASK) | ((uint32_t)a << COLOR_ALPHA_SHIFT);
	}

	static uint8_t getAlpha(Color c) { return (uint8_t)((c >> COLOR_ALPHA_SHIFT) & 0xFF); }
	static uint8_t getRed(Color c)   { return (uint8_t)((c >> COLOR_RED_SHIFT) & 0xFF);   }
	static uint8_t getGreen(Color c) { return (uint8_t)((c >> COLOR_GREEN_SHIFT) & 0xFF); }
	static uint8_t getBlue(Color c)  { return (uint8_t)((c >> COLOR_BLUE_SHIFT) & 0xFF);  }
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
		MiddleCenter = Middle | Center
	};
};

struct LineCap
{
	enum Enum : uint32_t
	{
		Butt,
		Round,
		Square,
	};
};

struct LineJoin
{
	enum Enum : uint32_t
	{
		Miter,
		Round,
		Bevel
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

BGFX_HANDLE(GradientHandle);
BGFX_HANDLE(ImagePatternHandle);
BGFX_HANDLE(ImageHandle);
BGFX_HANDLE(FontHandle);

struct Font
{
	FontHandle m_Handle;
	float m_Size;
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

class IRenderer
{
public:
	virtual ~IRenderer()
	{}

	virtual void BeginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio) = 0;
	virtual void EndFrame() = 0;

	virtual void BeginPath() = 0;
	virtual void MoveTo(float x, float y) = 0;
	virtual void LineTo(float x, float y) = 0;
	virtual void BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y) = 0;
	virtual void ArcTo(float x1, float y1, float x2, float y2, float radius) = 0;
	virtual void Rect(float x, float y, float w, float h) = 0;
	virtual void RoundedRect(float x, float y, float w, float h, float r) = 0;
	virtual void Circle(float cx, float cy, float radius) = 0;
	virtual void ClosePath() = 0;
	virtual void FillConvexPath(Color col, bool aa) = 0;
	virtual void FillConvexPath(GradientHandle gradient, bool aa) = 0;
	virtual void FillConvexPath(ImagePatternHandle img, bool aa) = 0;
	virtual void FillConcavePath(Color col, bool aa) = 0;
	virtual void StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap = LineCap::Butt, LineJoin::Enum lineJoin = LineJoin::Miter) = 0;

	virtual GradientHandle LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol) = 0;
	virtual GradientHandle BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol) = 0;
	virtual GradientHandle RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol) = 0;
	virtual ImagePatternHandle ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha) = 0;

	virtual ImageHandle CreateImageRGBA(int w, int h, uint32_t imageFlags, const uint8_t* data) = 0;
	virtual void UpdateImage(ImageHandle image, const uint8_t* data) = 0;
	virtual void GetImageSize(ImageHandle image, int* w, int* h) = 0;
	virtual void DeleteImage(ImageHandle image) = 0;
	virtual bool IsImageHandleValid(ImageHandle image) = 0;

	virtual void PushState() = 0;
	virtual void PopState() = 0;
	virtual void ResetScissor() = 0;
	virtual void Scissor(float x, float y, float w, float h) = 0;
	virtual bool IntersectScissor(float x, float y, float w, float h) = 0;
	virtual void LoadIdentity() = 0;
	virtual void Scale(float x, float y) = 0;
	virtual void Translate(float x, float y) = 0;
	virtual void Rotate(float ang_rad) = 0;
	virtual void SetGlobalAlpha(float alpha) = 0;

	virtual FontHandle LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size) = 0;
	virtual Font CreateFontWithSize(const char* name, float size) = 0;

	virtual void Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end) = 0;
	virtual void TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end) = 0;
	virtual float CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds) = 0;
	virtual void CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags) = 0;
	virtual float GetTextLineHeight(const Font& font, uint32_t alignment) = 0;
	virtual int TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakWidth, TextRow* rows, int numRows, uint32_t flags) = 0;
	virtual int TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* glyphs, int maxGlyphs) = 0;

	virtual Shape* CreateShape() = 0;
	virtual void DestroyShape(Shape* shape) = 0;
	virtual void SubmitShape(Shape* shape) = 0;
};
}

#endif
