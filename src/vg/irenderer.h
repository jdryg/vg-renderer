#ifndef VG_IRENDERER_H
#define VG_IRENDERER_H

#include <stdint.h>
#include "vg.h"

namespace vg
{
struct Shape;

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
	virtual void Polyline(const float* coords, uint32_t numPoints) = 0;
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

	virtual ImageHandle CreateImageRGBA(uint16_t w, uint16_t h, uint32_t imageFlags, const uint8_t* data) = 0;
	virtual void UpdateImage(ImageHandle image, const uint8_t* data) = 0;
	virtual void UpdateSubImage(ImageHandle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data) = 0;
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
	virtual void ApplyTransform(const float* mtx, bool pre = false) = 0;
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

	virtual Shape* CreateShape(uint32_t flags) = 0;
	virtual void DestroyShape(Shape* shape) = 0;
	virtual void SubmitShape(Shape* shape) = 0;
#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
	virtual void SubmitShape(Shape* shape, GetStringByIDFunc stringCallback, void* userData) = 0;
#endif

	virtual String* CreateString(const char* fontName, float fontSize, const char* text, const char* end) = 0;
	virtual void DestroyString(String* str) = 0;
	virtual void Text(String* str, uint32_t alignment, Color color, float x, float y) = 0;
};
}

#endif
