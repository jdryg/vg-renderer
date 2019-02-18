// WARNING: This file exists only to allow easier migration of existing code from the 
// IRenderer interface to vg::Context. It won't be maintained or extended to include
// all new functionality from vg::Context. New clients should use vg::Context instead
// of these structs/classes.
#ifndef VG_VGPP_H
#define VG_VGPP_H

#include <vg/vg.h>
#include <bx/allocator.h>

namespace vg
{
struct Shape;

struct Font
{
	FontHandle m_Handle;
	float m_Size;
};

class Renderer
{
public:
	bx::AllocatorI* m_Allocator; // This is needed in order to allocate Shape objects
	Context* m_Context;

	Renderer();
	Renderer(Context* ctx, bx::AllocatorI* allocator);
	~Renderer();

	bool init(uint8_t viewID, bx::AllocatorI* allocator);

	void BeginFrame(uint32_t canvasWidth, uint32_t canvasHeight, float devicePixelRatio);
	void EndFrame();

	void BeginPath();
	void MoveTo(float x, float y);
	void LineTo(float x, float y);
	void BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	void ArcTo(float x1, float y1, float x2, float y2, float radius);
	void Rect(float x, float y, float w, float h);
	void RoundedRect(float x, float y, float w, float h, float r);
	void RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
	void Circle(float cx, float cy, float radius);
	void Polyline(const float* coords, uint32_t numPoints);
	void ClosePath();
	void FillConvexPath(Color col, bool aa);
	void FillConvexPath(GradientHandle gradient, bool aa);
	void FillConvexPath(ImagePatternHandle img, bool aa);
	void FillConcavePath(Color col, bool aa);
	void StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap = LineCap::Butt, LineJoin::Enum lineJoin = LineJoin::Miter);
	void StrokePath(Color col, float width, uint32_t flags = StrokeFlags::ButtMiterAA);
	void StrokePath(vg::GradientHandle gradient, float width, bool aa, LineCap::Enum lineCap = LineCap::Butt, LineJoin::Enum lineJoin = LineJoin::Miter);
	void StrokePath(vg::GradientHandle gradient, float width, uint32_t flags = StrokeFlags::ButtMiterAA);
	void BeginClip(ClipRule::Enum rule);
	void EndClip();
	void ResetClip();

	GradientHandle LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	GradientHandle BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, vg::Color ocol);
	GradientHandle RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	ImagePatternHandle ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha);

	ImageHandle CreateImageRGBA(uint16_t w, uint16_t h, uint32_t imageFlags, const uint8_t* data);
	void UpdateImage(ImageHandle image, const uint8_t* data);
	void UpdateSubImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
	void GetImageSize(ImageHandle image, int* w, int* h);
	void DeleteImage(ImageHandle image);
	bool IsImageHandleValid(ImageHandle image);

	void PushState();
	void PopState();

	void ResetScissor();
	void Scissor(float x, float y, float w, float h);
	bool IntersectScissor(float x, float y, float w, float h);

	void LoadIdentity();
	void Scale(float x, float y);
	void Translate(float x, float y);
	void Rotate(float ang_rad);
	void ApplyTransform(const float* mtx, bool pre);

	void SetGlobalAlpha(float alpha);

	FontHandle LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size);
	Font CreateFontWithSize(const char* name, float size);

	void Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end);
	void TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end);
	float CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds);
	void CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags);
	float GetTextLineHeight(const Font& font, uint32_t alignment);
	int TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakWidth, TextRow* rows, int numRows, uint32_t flags);
	int TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* glyphs, int maxGlyphs);

	Shape* CreateShape(uint32_t flags);
	void DestroyShape(Shape* shape);
	void SubmitShape(Shape* shape);

private:
	bool m_OwnContext;
};

struct Shape
{
	CommandListRef m_CommandListRef;

	Shape(const CommandListRef& clr);
	~Shape();

	void Reset();

	void BeginPath();
	void MoveTo(float x, float y);
	void LineTo(float x, float y);
	void BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	void ArcTo(float x1, float y1, float x2, float y2, float radius);
	void Rect(float x, float y, float w, float h);
	void RoundedRect(float x, float y, float w, float h, float r);
	void RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
	void Circle(float cx, float cy, float radius);
	void ClosePath();
	void FillConvexPath(Color col, bool aa);
	void FillConvexPath(GradientHandle gradient, bool aa);
	void FillConvexPath(ImagePatternHandle img, bool aa);
	void FillConcavePath(Color col, bool aa);
	void StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap = LineCap::Butt, LineJoin::Enum lineJoin = LineJoin::Miter);

	void PushState();
	void PopState();
	void Scissor(float x, float y, float w, float h);
	void IntersectScissor(float x, float y, float w, float h);
	void Rotate(float ang_rad);
	void Translate(float x, float y);
	void Scale(float x, float y);
	void ApplyTransform(const float* transform);

	void BeginClip(ClipRule::Enum rule);
	void EndClip();
	void ResetClip();

	GradientHandle LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	GradientHandle BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
	GradientHandle RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	ImagePatternHandle ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha);

	void Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end);
	void TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end);
};

//////////////////////////////////////////////////////////////////////////
// Inline functions
//
inline Renderer::Renderer()
	: m_Allocator(nullptr)
	, m_Context(nullptr)
	, m_OwnContext(true)
{
}

inline Renderer::Renderer(Context* ctx, bx::AllocatorI* allocator)
	: m_Allocator(allocator)
	, m_Context(ctx)
	, m_OwnContext(false)
{
}

inline Renderer::~Renderer()
{
	if (m_OwnContext && m_Context) {
		destroyContext(m_Context);
	}
	m_Context = nullptr;

	m_Allocator = nullptr;
}

inline bool Renderer::init(uint8_t viewID, bx::AllocatorI* allocator)
{
	m_Allocator = allocator;
	m_Context = createContext(viewID, allocator);
	return m_Context != nullptr;
}

inline void Renderer::BeginFrame(uint32_t canvasWidth, uint32_t canvasHeight, float devicePixelRatio)
{
	beginFrame(m_Context, (uint16_t)canvasWidth, (uint16_t)canvasHeight, devicePixelRatio);
}

inline void Renderer::EndFrame()
{
	endFrame(m_Context);
}

inline void Renderer::BeginPath()
{
	beginPath(m_Context);
}

inline void Renderer::MoveTo(float x, float y)
{
	moveTo(m_Context, x, y);
}

inline void Renderer::LineTo(float x, float y)
{
	lineTo(m_Context, x, y);
}

inline void Renderer::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	cubicTo(m_Context, c1x, c1y, c2x, c2y, x, y);
}

inline void Renderer::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	arcTo(m_Context, x1, y1, x2, y2, radius);
}

inline void Renderer::Rect(float x, float y, float w, float h)
{
	rect(m_Context, x, y, w, h);
}

inline void Renderer::RoundedRect(float x, float y, float w, float h, float r)
{
	roundedRect(m_Context, x, y, w, h, r);
}

inline void Renderer::RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	roundedRectVarying(m_Context, x, y, w, h, rtl, rtr, rbr, rbl);
}

inline void Renderer::Circle(float cx, float cy, float radius)
{
	circle(m_Context, cx, cy, radius);
}

inline void Renderer::Polyline(const float* coords, uint32_t numPoints)
{
	polyline(m_Context, coords, numPoints);
}

inline void Renderer::ClosePath()
{
	closePath(m_Context);
}

inline void Renderer::FillConvexPath(Color col, bool aa)
{
	fillPath(m_Context, col, VG_FILL_FLAGS(PathType::Convex, aa));
}

inline void Renderer::FillConvexPath(GradientHandle gradient, bool aa)
{
	fillPath(m_Context, gradient, VG_FILL_FLAGS(PathType::Convex, aa));
}

inline void Renderer::FillConvexPath(ImagePatternHandle img, bool aa)
{
	fillPath(m_Context, img, Colors::White, VG_FILL_FLAGS(PathType::Convex, aa));
}

inline void Renderer::FillConcavePath(Color col, bool aa)
{
	fillPath(m_Context, col, VG_FILL_FLAGS(PathType::Concave, aa));
}

inline void Renderer::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	strokePath(m_Context, col, width, VG_STROKE_FLAGS(lineCap, lineJoin, aa));
}

inline void Renderer::StrokePath(Color col, float width, uint32_t flags)
{
	strokePath(m_Context, col, width, flags);
}

inline void Renderer::StrokePath(vg::GradientHandle gradient, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	strokePath(m_Context, gradient, width, VG_STROKE_FLAGS(lineCap, lineJoin, aa));
}

inline void Renderer::StrokePath(vg::GradientHandle gradient, float width, uint32_t flags)
{
	strokePath(m_Context, gradient, width, flags);
}

inline void Renderer::BeginClip(ClipRule::Enum rule)
{
	beginClip(m_Context, rule);
}

inline void Renderer::EndClip()
{
	endClip(m_Context);
}

inline void Renderer::ResetClip()
{
	resetClip(m_Context);
}

inline GradientHandle Renderer::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	return createLinearGradient(m_Context, sx, sy, ex, ey, icol, ocol);
}

inline GradientHandle Renderer::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, vg::Color ocol)
{
	return createBoxGradient(m_Context, x, y, w, h, r, f, icol, ocol);
}

inline GradientHandle Renderer::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	return createRadialGradient(m_Context, cx, cy, inr, outr, icol, ocol);
}

inline ImagePatternHandle Renderer::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	BX_UNUSED(alpha);
	return createImagePattern(m_Context, cx, cy, w, h, angle, image);
}

inline ImageHandle Renderer::CreateImageRGBA(uint16_t w, uint16_t h, uint32_t imageFlags, const uint8_t* data)
{
	return createImage(m_Context, w, h, imageFlags, data);
}

inline void Renderer::UpdateImage(ImageHandle image, const uint8_t* data)
{
	if (!isValid(image)) {
		return;
	}

	uint16_t w, h;
	getImageSize(m_Context, image, &w, &h);
	updateImage(m_Context, image, 0, 0, (uint16_t)w, (uint16_t)h, data);
}

inline void Renderer::UpdateSubImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data)
{
	updateImage(m_Context, image, x, y, w, h, data);
}

inline void Renderer::GetImageSize(ImageHandle image, int* iw, int* ih)
{
	uint16_t w, h;
	if (!getImageSize(m_Context, image, &w, &h)) {
		*iw = -1;
		*ih = -1;
	} else {
		*iw = (int)w;
		*ih = (int)h;
	}
}

inline void Renderer::DeleteImage(ImageHandle image)
{
	destroyImage(m_Context, image);
}

inline bool Renderer::IsImageHandleValid(ImageHandle image)
{
	return isImageValid(m_Context, image);
}

inline void Renderer::PushState()
{
	pushState(m_Context);
}

inline void Renderer::PopState()
{
	popState(m_Context);
}

inline void Renderer::ResetScissor()
{
	resetScissor(m_Context);
}

inline void Renderer::Scissor(float x, float y, float w, float h)
{
	setScissor(m_Context, x, y, w, h);
}

inline bool Renderer::IntersectScissor(float x, float y, float w, float h)
{
	return intersectScissor(m_Context, x, y, w, h);
}

inline void Renderer::LoadIdentity()
{
	transformIdentity(m_Context);
}

inline void Renderer::Scale(float x, float y)
{
	transformScale(m_Context, x, y);
}

inline void Renderer::Translate(float x, float y)
{
	transformTranslate(m_Context, x, y);
}

inline void Renderer::Rotate(float ang_rad)
{
	transformRotate(m_Context, ang_rad);
}

inline void Renderer::ApplyTransform(const float* mtx, bool pre)
{
	transformMult(m_Context, mtx, pre);
}

inline void Renderer::SetGlobalAlpha(float alpha)
{
	setGlobalAlpha(m_Context, alpha);
}

inline FontHandle Renderer::LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	return createFont(m_Context, name, (uint8_t*)data, size, 0);
}

inline Font Renderer::CreateFontWithSize(const char* name, float size)
{
	Font font;
	font.m_Handle = getFontByName(m_Context, name);
	font.m_Size = size;
	return font;
}

inline void Renderer::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* str, const char* end)
{
	text(m_Context, font.m_Handle, font.m_Size, alignment, color, x, y, str, end);
}

inline void Renderer::TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* str, const char* end)
{
	textBox(m_Context, font.m_Handle, font.m_Size, alignment, color, x, y, breakWidth, str, end, 0);
}

inline float Renderer::CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* str, const char* end, float* bounds)
{
	return measureText(m_Context, font.m_Handle, font.m_Size, alignment, x, y, str, end, bounds);
}

inline void Renderer::CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* str, const char* end, float* bounds, uint32_t flags)
{
	measureTextBox(m_Context, font.m_Handle, font.m_Size, alignment, x, y, breakWidth, str, end, bounds, flags);
}

inline float Renderer::GetTextLineHeight(const Font& font, uint32_t alignment)
{
	return getTextLineHeight(m_Context, font.m_Handle, font.m_Size, alignment);
}

inline int Renderer::TextBreakLines(const Font& font, uint32_t alignment, const char* str, const char* end, float breakWidth, TextRow* rows, int numRows, uint32_t flags)
{
	return textBreakLines(m_Context, font.m_Handle, font.m_Size, alignment, str, end, breakWidth, rows, numRows, flags);
}

inline int Renderer::TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* str, const char* end, GlyphPosition* glyphs, int maxGlyphs)
{
	return textGlyphPositions(m_Context, font.m_Handle, font.m_Size, alignment, x, y, str, end, glyphs, maxGlyphs);
}

inline Shape* Renderer::CreateShape(uint32_t flags)
{
	CommandListHandle handle = createCommandList(m_Context, flags);
	return BX_NEW(m_Allocator, Shape)(makeCommandListRef(m_Context, handle));
}

inline void Renderer::DestroyShape(Shape* shape)
{
	vg::destroyCommandList(m_Context, shape->m_CommandListRef.m_Handle);
	BX_DELETE(m_Allocator, shape);
}

inline void Renderer::SubmitShape(Shape* shape)
{
	VG_CHECK(shape->m_CommandListRef.m_Context == m_Context, "vg::Context mismatch");
	submitCommandList(m_Context, shape->m_CommandListRef.m_Handle);
}

// Shape
inline Shape::Shape(const CommandListRef& clr)
	: m_CommandListRef(clr)
{
}

inline Shape::~Shape()
{
}

inline void Shape::Reset()
{
	clReset(m_CommandListRef);
}

inline void Shape::BeginPath()
{
	clBeginPath(m_CommandListRef);
}

inline void Shape::MoveTo(float x, float y)
{
	clMoveTo(m_CommandListRef, x, y);
}

inline void Shape::LineTo(float x, float y)
{
	clLineTo(m_CommandListRef, x, y);
}

inline void Shape::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	clCubicTo(m_CommandListRef, c1x, c1y, c2x, c2y, x, y);
}

inline void Shape::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	clArcTo(m_CommandListRef, x1, y1, x2, y2, radius);
}

inline void Shape::Rect(float x, float y, float w, float h)
{
	clRect(m_CommandListRef, x, y, w, h);
}

inline void Shape::RoundedRect(float x, float y, float w, float h, float r)
{
	clRoundedRect(m_CommandListRef, x, y, w, h, r);
}

inline void Shape::RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	clRoundedRectVarying(m_CommandListRef, x, y, w, h, rtl, rtr, rbr, rbl);
}

inline void Shape::Circle(float cx, float cy, float radius)
{
	clCircle(m_CommandListRef, cx, cy, radius);
}

inline void Shape::ClosePath()
{
	clClosePath(m_CommandListRef);
}

inline void Shape::FillConvexPath(Color col, bool aa)
{
	clFillPath(m_CommandListRef, col, VG_FILL_FLAGS(PathType::Convex, aa));
}

inline void Shape::FillConvexPath(GradientHandle gradient, bool aa)
{
	clFillPath(m_CommandListRef, gradient, VG_FILL_FLAGS(PathType::Convex, aa));
}

inline void Shape::FillConvexPath(ImagePatternHandle img, bool aa)
{
	clFillPath(m_CommandListRef, img, Colors::White, VG_FILL_FLAGS(PathType::Convex, aa));
}

inline void Shape::FillConcavePath(Color col, bool aa)
{
	clFillPath(m_CommandListRef, col, VG_FILL_FLAGS(PathType::Concave, aa));
}

inline void Shape::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	clStrokePath(m_CommandListRef, col, width, VG_STROKE_FLAGS(lineCap, lineJoin, aa));
}

inline void Shape::PushState()
{
	clPushState(m_CommandListRef);
}

inline void Shape::PopState()
{
	clPopState(m_CommandListRef);
}

inline void Shape::Scissor(float x, float y, float w, float h)
{
	clSetScissor(m_CommandListRef, x, y, w, h);
}

inline void Shape::IntersectScissor(float x, float y, float w, float h)
{
	clIntersectScissor(m_CommandListRef, x, y, w, h);
}

inline void Shape::Rotate(float ang_rad)
{
	clTransformRotate(m_CommandListRef, ang_rad);
}

inline void Shape::Translate(float x, float y)
{
	clTransformTranslate(m_CommandListRef, x, y);
}

inline void Shape::Scale(float x, float y)
{
	clTransformScale(m_CommandListRef, x, y);
}

inline void Shape::ApplyTransform(const float* transform)
{
	clTransformMult(m_CommandListRef, transform, false);
}

inline void Shape::BeginClip(ClipRule::Enum rule)
{
	clBeginClip(m_CommandListRef, rule);
}

inline void Shape::EndClip()
{
	clEndClip(m_CommandListRef);
}

inline void Shape::ResetClip()
{
	clResetClip(m_CommandListRef);
}

inline GradientHandle Shape::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	return clCreateLinearGradient(m_CommandListRef, sx, sy, ex, ey, icol, ocol);
}

inline GradientHandle Shape::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	return clCreateBoxGradient(m_CommandListRef, x, y, w, h, r, f, icol, ocol);
}

inline GradientHandle Shape::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	return clCreateRadialGradient(m_CommandListRef, cx, cy, inr, outr, icol, ocol);
}

inline ImagePatternHandle Shape::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	BX_UNUSED(alpha);
	return clCreateImagePattern(m_CommandListRef, cx, cy, w, h, angle, image);
}

inline void Shape::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* str, const char* end)
{
	TextConfig cfg = { font.m_Handle, font.m_Size, alignment, color };
	clText(m_CommandListRef, cfg, x, y, str, end);
}

inline void Shape::TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* str, const char* end)
{
	TextConfig cfg = { font.m_Handle, font.m_Size, alignment, color };
	clTextBox(m_CommandListRef, cfg, x, y, breakWidth, str, end, 0);
}
}

#endif
