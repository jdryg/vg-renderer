#if VG_CONFIG_DEBUG
#define BX_TRACE(_format, ...) \
	do { \
		bx::debugPrintf(BX_FILE_LINE_LITERAL "SVGRenderer " _format "\n", ##__VA_ARGS__); \
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

#include "svg_renderer.h"
#include "shape.h"

#include <bx/allocator.h>
#include <bx/math.h>
#include <bx/mutex.h>
#include <bx/simd_t.h>
#include <bx/handlealloc.h>
#include <memory.h>
#include <math.h>
#include <float.h>

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

void colorToSVGString(Color c, char* str, uint32_t maxLen)
{
	uint8_t r = ColorRGBA::getRed(c);
	uint8_t g = ColorRGBA::getGreen(c);
	uint8_t b = ColorRGBA::getBlue(c);
	uint8_t a = ColorRGBA::getAlpha(c);
	if (a != 255) {
		bx::snprintf(str, maxLen, "rgba(%d, %d, %d, %f)", r, g, b, (float)a / 255.0f);
	} else {
		bx::snprintf(str, maxLen, "#%02X%02X%02X", r, g, b);
	}
}

struct State
{
	float m_TransformMtx[6];
	float m_ScissorRect[4];
	float m_GlobalAlpha;
	float m_FontScale;
	float m_AvgScale;
};

struct SVGShapeType
{
	enum Enum
	{
		Path,
		Rect,
		Circle,
		Text
	};
};

struct SVGPathString
{
	char* m_Str;
	char* m_Ptr;
	uint32_t m_Capacity;
};

struct SVGShape
{
	SVGShapeType::Enum m_Type;
	float m_Attrs[5]; // Circle: x, y, r, Rect: x, y, w, h, r
	SVGPathString m_Path; // Path
	float m_TransformMtx[6];
};

namespace svg {
struct Context
{
	bx::AllocatorI* m_Allocator;
	char* m_Filename;

	FILE* m_File;

	SVGShape* m_PathShapes;
	uint32_t m_PathShapeCapacity;
	uint32_t m_NumPathShapes;
	Color m_FillColor;
	Color m_StrokeColor;
	float m_StrokeWidth;
	LineCap::Enum m_StrokeLineCap;
	LineJoin::Enum m_StrokeLineJoin;

	uint16_t m_WinWidth;
	uint16_t m_WinHeight;

	State m_StateStack[MAX_STATE_STACK_SIZE];
	uint32_t m_CurStateID;

	Context(bx::AllocatorI* allocator, const char* filename);
	~Context();

	// Helpers...
	inline State* getState()               { return &m_StateStack[m_CurStateID]; }

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
	void fillPath(Color col);
	void fillPath(GradientHandle gradient);
	void fillPath(ImagePatternHandle img);
	void strokePath(Color col, float width, LineCap::Enum lineCap, LineJoin::Enum lineJoin);
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

	// Shapes
	Shape* createShape(uint32_t flags);
	void destroyShape(Shape* shape);
	void submitShape(Shape* shape, GetStringByIDFunc* stringCallback, void* userData);

	// Fonts
	FontHandle loadFontFromMemory(const char* name, const uint8_t* data, uint32_t size);
	FontHandle getFontHandleByName(const char* name);

	void flushPathShapes();
	SVGShape* allocPathShape(SVGShapeType::Enum type);
	SVGShape* getLastPathShape();
	void pathAppend(SVGShape* shape, const char* str);
};
}

//////////////////////////////////////////////////////////////////////////
// SVGRenderer
//
SVGRenderer::SVGRenderer() : m_Context(nullptr)
{
}

SVGRenderer::~SVGRenderer()
{
	if (m_Context) {
		BX_DELETE(m_Context->m_Allocator, m_Context);
		m_Context = nullptr;
	}
}

bool SVGRenderer::init(const char* filename, bx::AllocatorI* allocator)
{
	m_Context = (svg::Context*)BX_NEW(allocator, svg::Context)(allocator, filename);
	if (!m_Context) {
		return false;
	}

	if (!m_Context->init()) {
		BX_DELETE(allocator, m_Context);
		return false;
	}

	return true;
}

void SVGRenderer::BeginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio)
{
	m_Context->beginFrame(windowWidth, windowHeight, devicePixelRatio);
}

void SVGRenderer::EndFrame()
{
	m_Context->endFrame();
}

void SVGRenderer::BeginPath()
{
	m_Context->beginPath();
}

void SVGRenderer::MoveTo(float x, float y)
{
	m_Context->moveTo(x, y);
}

void SVGRenderer::LineTo(float x, float y)
{
	m_Context->lineTo(x, y);
}

void SVGRenderer::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	m_Context->bezierTo(c1x, c1y, c2x, c2y, x, y);
}

void SVGRenderer::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	m_Context->arcTo(x1, y1, x2, y2, radius);
}

void SVGRenderer::Rect(float x, float y, float w, float h)
{
	m_Context->rect(x, y, w, h);
}

void SVGRenderer::RoundedRect(float x, float y, float w, float h, float r)
{
	m_Context->roundedRect(x, y, w, h, r);
}

void SVGRenderer::Circle(float cx, float cy, float r)
{
	m_Context->circle(cx, cy, r);
}

void SVGRenderer::Polyline(const float* coords, uint32_t numPoints)
{
	m_Context->polyline((const Vec2*)coords, numPoints);
}

void SVGRenderer::ClosePath()
{
	m_Context->closePath();
}

void SVGRenderer::FillConvexPath(Color col, bool aa)
{
	BX_UNUSED(aa);
	m_Context->fillPath(col);
}

void SVGRenderer::FillConvexPath(GradientHandle gradient, bool aa)
{
	BX_UNUSED(aa);
	m_Context->fillPath(gradient);
}

void SVGRenderer::FillConvexPath(ImagePatternHandle img, bool aa)
{
	BX_UNUSED(aa);
	m_Context->fillPath(img);
}

void SVGRenderer::FillConcavePath(Color col, bool aa)
{
	BX_UNUSED(aa);
	m_Context->fillPath(col);
}

void SVGRenderer::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	BX_UNUSED(aa);
	m_Context->strokePath(col, width, lineCap, lineJoin);
}

GradientHandle SVGRenderer::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	return m_Context->createLinearGradient(sx, sy, ex, ey, icol, ocol);
}

GradientHandle SVGRenderer::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	return m_Context->createBoxGradient(x, y, w, h, r, f, icol, ocol);
}

GradientHandle SVGRenderer::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	return m_Context->createRadialGradient(cx, cy, inr, outr, icol, ocol);
}

ImagePatternHandle SVGRenderer::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	return m_Context->createImagePattern(cx, cy, w, h, angle, image, alpha);
}

ImageHandle SVGRenderer::CreateImageRGBA(uint16_t w, uint16_t h, uint32_t imageFlags, const uint8_t* data)
{
	BX_UNUSED(w, h, imageFlags, data);
	return VG_INVALID_HANDLE;
}

void SVGRenderer::UpdateImage(ImageHandle image, const uint8_t* data)
{
	BX_UNUSED(image, data);
}

void SVGRenderer::UpdateSubImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data)
{
	BX_UNUSED(image, x, y, w, h, data);
}

void SVGRenderer::GetImageSize(ImageHandle image, int* w, int* h)
{
	BX_UNUSED(image);
	if (w) { *w = 0; }
	if (h) { *h = 0; }
}

void SVGRenderer::DeleteImage(ImageHandle image)
{
	BX_UNUSED(image);
}

bool SVGRenderer::IsImageHandleValid(ImageHandle image)
{
	BX_UNUSED(image);
	return false;
}

void SVGRenderer::PushState()
{
	m_Context->pushState();
}

void SVGRenderer::PopState()
{
	m_Context->popState();
}

void SVGRenderer::ResetScissor()
{
	m_Context->resetScissor();
}

void SVGRenderer::Scissor(float x, float y, float w, float h)
{
	m_Context->setScissor(x, y, w, h);
}

bool SVGRenderer::IntersectScissor(float x, float y, float w, float h)
{
	return m_Context->intersectScissor(x, y, w, h);
}

void SVGRenderer::LoadIdentity()
{
	m_Context->transformIdentity();
}

void SVGRenderer::Scale(float x, float y)
{
	m_Context->transformScale(x, y);
}

void SVGRenderer::Translate(float x, float y)
{
	m_Context->transformTranslate(x, y);
}

void SVGRenderer::Rotate(float ang_rad)
{
	m_Context->transformRotate(ang_rad);
}

void SVGRenderer::ApplyTransform(const float* mtx, bool pre)
{
	m_Context->transformMult(mtx, pre);
}

void SVGRenderer::SetGlobalAlpha(float alpha)
{
	m_Context->setGlobalAlpha(alpha);
}

void SVGRenderer::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	m_Context->text(font, alignment, color, x, y, text, end);
}

void SVGRenderer::TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end)
{
	m_Context->textBox(font, alignment, color, x, y, breakWidth, text, end);
}

float SVGRenderer::CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds)
{
	return m_Context->calcTextBounds(font, alignment, x, y, text, end, bounds);
}

void SVGRenderer::CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	m_Context->calcTextBoxBounds(font, alignment, x, y, breakWidth, text, end, bounds, flags);
}

float SVGRenderer::GetTextLineHeight(const Font& font, uint32_t alignment)
{
	return m_Context->getTextLineHeight(font, alignment);
}

int SVGRenderer::TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	return m_Context->textBreakLines(font, alignment, text, end, breakRowWidth, rows, maxRows, flags);
}

int SVGRenderer::TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions)
{
	return m_Context->textGlyphPositions(font, alignment, x, y, text, end, positions, maxPositions);
}

FontHandle SVGRenderer::LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	return m_Context->loadFontFromMemory(name, data, size);
}

Font SVGRenderer::CreateFontWithSize(const char* name, float size)
{
	Font f;
	f.m_Handle = m_Context->getFontHandleByName(name);
	f.m_Size = size;
	return f;
}

Shape* SVGRenderer::CreateShape(uint32_t flags)
{
	return m_Context->createShape(flags);
}

void SVGRenderer::DestroyShape(Shape* shape)
{
	m_Context->destroyShape(shape);
}

void SVGRenderer::SubmitShape(Shape* shape)
{
	m_Context->submitShape(shape, nullptr, nullptr);
}

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
void SVGRenderer::SubmitShape(Shape* shape, GetStringByIDFunc stringCallback, void* userData)
{
	m_Context->submitShape(shape, &stringCallback, userData);
}
#endif // VG_CONFIG_SHAPE_DYNAMIC_TEXT

//////////////////////////////////////////////////////////////////////////
// Context
//
namespace svg
{
Context::Context(bx::AllocatorI* allocator, const char* filename) : 
	m_Allocator(allocator), 
	m_PathShapes(nullptr), 
	m_File(nullptr),
	m_NumPathShapes(0),
	m_PathShapeCapacity(0),
	m_CurStateID(0),
	m_StrokeWidth(0.0f),
	m_StrokeLineCap(LineCap::Butt),
	m_StrokeLineJoin(LineJoin::Miter),
	m_StrokeColor(ColorRGBA::Transparent),
	m_FillColor(ColorRGBA::Transparent)
{
	int len = bx::strLen(filename);
	m_Filename = (char*)BX_ALLOC(allocator, len + 1);
	bx::memCopy(m_Filename, filename, len);
	m_Filename[len] = 0;
}

Context::~Context()
{
	BX_FREE(m_Allocator, m_Filename);
	m_Filename = nullptr;
	m_Allocator = nullptr;
}

bool Context::init()
{
	return true;
}

void Context::beginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio)
{
	BX_UNUSED(devicePixelRatio);
	BX_CHECK(m_File == nullptr, "!!!");

	m_WinWidth = (uint16_t)windowWidth;
	m_WinHeight = (uint16_t)windowHeight;

	BX_CHECK(m_CurStateID == 0, "State stack hasn't been properly reset in the previous frame");
	resetScissor();
	transformIdentity();

	m_File = fopen(m_Filename, "w");

	fprintf(m_File, "<svg width=\"%u\" height=\"%u\" xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\">\n", windowWidth, windowHeight);
}

void Context::endFrame()
{
	flushPathShapes();

	fprintf(m_File, "</svg>\n");

	BX_CHECK(m_File != nullptr, "!!!");
	fclose(m_File);
	m_File = nullptr;
}

void Context::beginPath()
{
	flushPathShapes();

	m_FillColor = ColorRGBA::Transparent;
	m_StrokeColor = ColorRGBA::Transparent;
}

void Context::moveTo(float x, float y)
{
	SVGShape* shape = allocPathShape(SVGShapeType::Path);

	char str[256];
	bx::snprintf(str, 256, "M %f %f ", x, y);
	pathAppend(shape, str);
}

void Context::lineTo(float x, float y)
{
	SVGShape* shape = getLastPathShape();
	BX_CHECK(shape && shape->m_Type == SVGShapeType::Path, "");

	char str[256];
	bx::snprintf(str, 256, "L %f %f ", x, y);
	pathAppend(shape, str);
}

void Context::bezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	SVGShape* shape = getLastPathShape();
	BX_CHECK(shape && shape->m_Type == SVGShapeType::Path, "");

	char str[256];
	bx::snprintf(str, 256, "C %f %f, %f %f, %f %f ", c1x, c1y, c2x, c2y, x, y);
	pathAppend(shape, str);
}

void Context::arcTo(float x1, float y1, float x2, float y2, float radius)
{
	BX_UNUSED(x1, y1, x2, y2, radius);
	// TODO: 
}

void Context::rect(float x, float y, float w, float h)
{
	SVGShape* shape = allocPathShape(SVGShapeType::Rect);
	shape->m_Attrs[0] = x;
	shape->m_Attrs[1] = y;
	shape->m_Attrs[2] = w;
	shape->m_Attrs[3] = h;
	shape->m_Attrs[4] = 0.0f;
}

void Context::roundedRect(float x, float y, float w, float h, float r)
{
	SVGShape* shape = allocPathShape(SVGShapeType::Rect);
	shape->m_Attrs[0] = x;
	shape->m_Attrs[1] = y;
	shape->m_Attrs[2] = w;
	shape->m_Attrs[3] = h;
	shape->m_Attrs[4] = r;
}

void Context::circle(float cx, float cy, float r)
{
	SVGShape* shape = allocPathShape(SVGShapeType::Circle);
	shape->m_Attrs[0] = cx;
	shape->m_Attrs[1] = cy;
	shape->m_Attrs[2] = r;
}

void Context::polyline(const Vec2* coords, uint32_t numPoints)
{
	BX_CHECK(numPoints > 1, "");

	bool moveToFirst = false;
	SVGShape* shape = getLastPathShape();
	if (!shape || shape->m_Type != SVGShapeType::Path) {
		shape = allocPathShape(SVGShapeType::Path);
		moveToFirst = true;
	}

	BX_CHECK(shape && shape->m_Type == SVGShapeType::Path, "");

	if (shape->m_Path.m_Ptr == shape->m_Path.m_Str) {
		// Empty path.
		moveToFirst = true;
	}

	uint32_t startID = 0;
	if (moveToFirst) {
		moveTo(coords[0].x, coords[0].y);
		startID = 1;
	}

	for (uint32_t i = startID; i < numPoints; ++i) {
		lineTo(coords[i].x, coords[i].y);
	}
}

void Context::closePath()
{
	SVGShape* shape = getLastPathShape();
	BX_CHECK(shape && shape->m_Type == SVGShapeType::Path, "");

	pathAppend(shape, "Z");
}

void Context::fillPath(Color col)
{
	m_FillColor = col;

	// Copy current transform to all paths because I don't know when flush() will be called.
	const State* state = getState();
	const float* mtx = &state->m_TransformMtx[0];
	const uint32_t n = m_NumPathShapes;
	for (uint32_t i = 0; i < n; ++i) {
		bx::memCopy(m_PathShapes[i].m_TransformMtx, mtx, sizeof(float) * 6);
	}
}

void Context::fillPath(GradientHandle gradient)
{
	// TODO: 
	BX_UNUSED(gradient);
}

void Context::fillPath(ImagePatternHandle img)
{
	// TODO: 
	BX_UNUSED(img);
}

void Context::strokePath(Color col, float width, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	m_StrokeColor = col;
	m_StrokeWidth = width;
	m_StrokeLineCap = lineCap;
	m_StrokeLineJoin = lineJoin;

	// Copy current transform to all paths because I don't know when flush() will be called.
	const State* state = getState();
	const float* mtx = &state->m_TransformMtx[0];
	const uint32_t n = m_NumPathShapes;
	for (uint32_t i = 0; i < n; ++i) {
		bx::memCopy(m_PathShapes[i].m_TransformMtx, mtx, sizeof(float) * 6);
	}
}

GradientHandle Context::createLinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	// TODO: 
	return VG_INVALID_HANDLE;
}

GradientHandle Context::createBoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	// TODO: 
	return VG_INVALID_HANDLE;
}

GradientHandle Context::createRadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	// TODO: 
	return VG_INVALID_HANDLE;
}

ImagePatternHandle Context::createImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	// TODO: 
	return VG_INVALID_HANDLE;
}

void Context::text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	flushPathShapes();

	const State* state = getState();
	const float* mtx = &state->m_TransformMtx[0];

	char fillColorStr[32];
	colorToSVGString(color, fillColorStr, 32);

	const char* anchorStr = "start";
	if (alignment & TextAlign::Center) {
		anchorStr = "middle";
	} else if (alignment & TextAlign::Right) {
		anchorStr = "end";
	}

	const char* baselineStr = "after-edge";
	if (alignment & TextAlign::Top) {
		baselineStr = "before-edge";
	} else if (alignment & TextAlign::Middle) {
		baselineStr = "middle";
	}

	fprintf(m_File, "<text x=\"%f\" y=\"%f\" stroke=\"none\" fill=\"%s\" transform=\"matrix(%f %f %f %f %f %f)\" font-size=\"%f\" font-family=\"monospace\" text-anchor=\"%s\" alignment-baseline=\"%s\">",
		x, 
		y, 
		fillColorStr, 
		mtx[0], 
		mtx[1], 
		mtx[2], 
		mtx[3], 
		mtx[4], 
		mtx[5],
		font.m_Size,
		anchorStr,
		baselineStr);
	
	if (end == nullptr) {
		fprintf(m_File, "%s", text);
	} else {
		fprintf(m_File, "%.*s", end - text, text);
	}

	fprintf(m_File, "</text>\n");
}

void Context::textBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end)
{
	// TODO: 
}

float Context::calcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds)
{
	// TODO: 
	return 0.0f;
}

void Context::calcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	// TODO: 
}

float Context::getTextLineHeight(const Font& font, uint32_t alignment)
{
	// TODO: 
	return 0.0f;
}

int Context::textBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	// TODO: 
	return 0;
}

int Context::textGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* positions, int maxPositions)
{
	// TODO: 
	return 0;
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
}

void Context::resetScissor()
{
	State* state = getState();
	state->m_ScissorRect[0] = state->m_ScissorRect[1] = 0.0f;
	state->m_ScissorRect[2] = (float)m_WinWidth;
	state->m_ScissorRect[3] = (float)m_WinHeight;
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
}

bool Context::intersectScissor(float x, float y, float w, float h)
{
	State* state = getState();
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

	return state->m_ScissorRect[2] >= 1.0f && state->m_ScissorRect[3] >= 1.0f;
}

void Context::setGlobalAlpha(float alpha)
{
	getState()->m_GlobalAlpha = alpha;
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
	const float c = cosf(ang_rad);
	const float s = sinf(ang_rad);

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

Shape* Context::createShape(uint32_t flags)
{
	// TODO: 
	return nullptr;
}

void Context::destroyShape(Shape* shape)
{
	// TODO: 
}

void Context::submitShape(Shape* shape, GetStringByIDFunc* stringCallback, void* userData)
{
	// TODO: 
}

FontHandle Context::loadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	// TODO: 
	return VG_INVALID_HANDLE;
}

FontHandle Context::getFontHandleByName(const char* name)
{
	// TODO: 
	return VG_INVALID_HANDLE;
}

void Context::flushPathShapes()
{
	const uint32_t n = m_NumPathShapes;
	if (n == 0) {
		return;
	}

	char strokeStyle[256];
	char fillStyle[256];

	if (ColorRGBA::getAlpha(m_StrokeColor) != 0) {
		char colorStr[32];
		colorToSVGString(m_StrokeColor, colorStr, 32);

		const char* lineCapStr = 
			m_StrokeLineCap == LineCap::Butt ? "butt" :
			m_StrokeLineCap == LineCap::Square ? "square" :
			m_StrokeLineCap == LineCap::Round ? "round" :
			"unknown";

		const char* lineJoinStr =
			m_StrokeLineJoin == LineJoin::Bevel ? "bevel" :
			m_StrokeLineJoin == LineJoin::Miter ? "miter" :
			m_StrokeLineJoin == LineJoin::Round ? "round" :
			"unknown";

		bx::snprintf(strokeStyle, 256, "stroke=\"%s\" stroke-width=\"%f\" stroke-linecap=\"%s\" stroke-linejoin=\"%s\" ", colorStr, m_StrokeWidth, lineCapStr, lineJoinStr);
	} else {
		bx::snprintf(strokeStyle, 256, "stroke=\"none\" ");
	}

	if (ColorRGBA::getAlpha(m_FillColor) != 0) {
		char colorStr[32];
		colorToSVGString(m_FillColor, colorStr, 32);
		bx::snprintf(fillStyle, 256, "fill=\"%s\" ", colorStr);
	} else {
		bx::snprintf(fillStyle, 256, "fill=\"none\" ");
	}

	for (uint32_t i = 0; i < n; ++i) {
		SVGShape* shape = &m_PathShapes[i];

		if (shape->m_Type == SVGShapeType::Rect) {
			const float x = shape->m_Attrs[0];
			const float y = shape->m_Attrs[1];
			const float w = shape->m_Attrs[2];
			const float h = shape->m_Attrs[3];
			const float r = shape->m_Attrs[4];
			const float* mtx = &shape->m_TransformMtx[0];

			fprintf(m_File, "\t<rect x=\"%f\" y=\"%f\" width=\"%f\" height=\"%f\" rx=\"%f\" ry=\"%f\" transform=\"matrix(%f %f %f %f %f %f)\" %s %s />\n", x, y, w, h, r, r,
				mtx[0], mtx[1], mtx[2], mtx[3], mtx[4], mtx[5],
				strokeStyle, fillStyle);
		} else if (shape->m_Type == SVGShapeType::Circle) {
			const float x = shape->m_Attrs[0];
			const float y = shape->m_Attrs[1];
			const float r = shape->m_Attrs[2];
			const float* mtx = &shape->m_TransformMtx[0];

			fprintf(m_File, "\t<circle cx=\"%f\" cy=\"%f\" r=\"%f\" transform=\"matrix(%f %f %f %f %f %f)\" %s %s />\n", x, y, r,
				mtx[0], mtx[1], mtx[2], mtx[3], mtx[4], mtx[5],
				strokeStyle, fillStyle);
		} else if (shape->m_Type == SVGShapeType::Path) {
			const char* pathStr = shape->m_Path.m_Str;
			const float* mtx = &shape->m_TransformMtx[0];

			fprintf(m_File, "\t<path d=\"%s\" transform=\"matrix(%f %f %f %f %f %f)\" %s %s />\n", pathStr,
				mtx[0], mtx[1], mtx[2], mtx[3], mtx[4], mtx[5],
				strokeStyle, fillStyle);
		} else {
			BX_CHECK(false, "Unknown shape type");
		}
	}

	m_NumPathShapes = 0;
}

SVGShape* Context::allocPathShape(SVGShapeType::Enum type)
{
	if (m_NumPathShapes + 1 > m_PathShapeCapacity) {
		uint32_t oldCapacity = m_PathShapeCapacity;
		m_PathShapeCapacity += 32;
		m_PathShapes = (SVGShape*)BX_REALLOC(m_Allocator, m_PathShapes, sizeof(SVGShape) * m_PathShapeCapacity);

		bx::memSet(&m_PathShapes[oldCapacity], 0, sizeof(SVGShape) * 32);
	}

	SVGShape* shape = &m_PathShapes[m_NumPathShapes];
	shape->m_Type = type;
	bx::memSet(shape->m_Attrs, 0, sizeof(float) * BX_COUNTOF(shape->m_Attrs));
	shape->m_Path.m_Ptr = shape->m_Path.m_Str;
	if (shape->m_Path.m_Ptr) {
		*shape->m_Path.m_Ptr = 0;
	}

	++m_NumPathShapes;
	return shape;
}

SVGShape* Context::getLastPathShape()
{
	if (m_NumPathShapes > 0) {
		return &m_PathShapes[m_NumPathShapes - 1];
	}

	return nullptr;
}

void Context::pathAppend(SVGShape* path, const char* str)
{
	BX_CHECK(path->m_Type == SVGShapeType::Path, "!!!");

	int len = bx::strLen(str);
	SVGPathString* pathStr = &path->m_Path;
	
	uint32_t pos = (uint32_t)(pathStr->m_Ptr - pathStr->m_Str);
	if (pos + len + 1> pathStr->m_Capacity) {
		pathStr->m_Capacity = pos + len + 1;
		pathStr->m_Str = (char*)BX_REALLOC(m_Allocator, pathStr->m_Str, pathStr->m_Capacity);
		pathStr->m_Ptr = pathStr->m_Str + pos;
	}

	bx::memCopy(pathStr->m_Ptr, str, len);
	pathStr->m_Ptr += len;
	*pathStr->m_Ptr = 0;
}
} // namespace svg
} // namespace vg
