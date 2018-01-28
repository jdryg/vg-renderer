#ifndef VG_SVG_RENDERER_H
#define VG_SVG_RENDERER_H

#include "irenderer.h"

namespace bx
{
struct AllocatorI;
}

namespace vg
{
namespace svg
{
	struct Context;
}

class SVGRenderer : public IRenderer
{
public:
	SVGRenderer();
	virtual ~SVGRenderer();

	bool init(const char* filename, bx::AllocatorI* allocator);

	// IRenderer interface...
	virtual void BeginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio);
	virtual void EndFrame();

	virtual void BeginPath();
	virtual void MoveTo(float x, float y);
	virtual void LineTo(float x, float y);
	virtual void BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	virtual void ArcTo(float x1, float y1, float x2, float y2, float radius);
	virtual void Rect(float x, float y, float w, float h);
	virtual void RoundedRect(float x, float y, float w, float h, float r);
	virtual void RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr);
	virtual void Circle(float cx, float cy, float radius);
	virtual void Polyline(const float* coords, uint32_t numPoints);
	virtual void ClosePath();
	virtual void FillConvexPath(Color col, bool aa);
	virtual void FillConvexPath(GradientHandle gradient, bool aa);
	virtual void FillConvexPath(ImagePatternHandle img, bool aa);
	virtual void FillConcavePath(Color col, bool aa);
	virtual void StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin);
	virtual void BeginClip(ClipRule::Enum fillRule);
	virtual void EndClip();
	virtual void ResetClip();

	virtual GradientHandle LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	virtual GradientHandle BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, vg::Color ocol);
	virtual GradientHandle RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	virtual ImagePatternHandle ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha);

	virtual ImageHandle CreateImageRGBA(uint16_t w, uint16_t h, uint32_t imageFlags, const uint8_t* data);
	virtual void UpdateImage(ImageHandle image, const uint8_t* data);
	virtual void UpdateSubImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
	virtual void GetImageSize(ImageHandle image, int* w, int* h);
	virtual void DeleteImage(ImageHandle image);
	virtual bool IsImageHandleValid(ImageHandle image);

	virtual void PushState();
	virtual void PopState();

	virtual void ResetScissor();
	virtual void Scissor(float x, float y, float w, float h);
	virtual bool IntersectScissor(float x, float y, float w, float h);
	
	virtual void LoadIdentity();
	virtual void Scale(float x, float y);
	virtual void Translate(float x, float y);
	virtual void Rotate(float ang_rad);
	virtual void ApplyTransform(const float* mtx, bool pre);

	virtual void SetGlobalAlpha(float alpha);

	virtual FontHandle LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size);
	virtual Font CreateFontWithSize(const char* name, float size);

	virtual void Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end);
	virtual void TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end);
	virtual float CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds);
	virtual void CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags);
	virtual float GetTextLineHeight(const Font& font, uint32_t alignment);
	virtual int TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakWidth, TextRow* rows, int numRows, uint32_t flags);
	virtual int TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* glyphs, int maxGlyphs);

	virtual Shape* CreateShape(uint32_t flags);
	virtual void DestroyShape(Shape* shape);
	virtual void SubmitShape(Shape* shape);
#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
	virtual void SubmitShape(Shape* shape, GetStringByIDFunc stringCallback, void* userData);
#endif

	virtual String* CreateString(const char* fontName, float fontSize, const char* text, const char* end);
	virtual void DestroyString(String* str);
	virtual void Text(String* str, uint32_t alignment, Color color, float x, float y);

private:
	svg::Context* m_Context;
};
}

#endif
