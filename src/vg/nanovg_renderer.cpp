#include "nanovg_renderer.h"
#include "../nanovg/nanovg.h"
#include <bx/allocator.h>
#include <assert.h>

#define MAX_PAINT_HANDLES 256

namespace vg
{
NanoVGRenderer::NanoVGRenderer() : 
	m_Context(nullptr), 
	m_Paints(nullptr),
	m_NextPaintID(0)
{
}

NanoVGRenderer::~NanoVGRenderer()
{
	if (m_Context) {
		nvgDelete(m_Context);
		m_Context = nullptr;
	}

	BX_FREE(m_Allocator, m_Paints);
}

bool NanoVGRenderer::init(bool edgeAA, uint8_t viewID, bx::AllocatorI* allocator)
{
	m_Allocator = allocator;

	m_Context = nvgCreate(edgeAA ? 1 : 0, viewID, allocator);
	if (!m_Context) {
		return false;
	}

	m_Paints = (NVGpaint*)BX_ALLOC(allocator, sizeof(NVGpaint) * MAX_PAINT_HANDLES);
	
	return true;
}

void NanoVGRenderer::BeginFrame(uint32_t windowWidth, uint32_t windowHeight, float devicePixelRatio)
{
	assert(m_Context != nullptr);
	m_NextPaintID = 0;
	nvgBeginFrame(m_Context, (int)windowWidth, (int)windowHeight, devicePixelRatio);
}

void NanoVGRenderer::EndFrame()
{
	assert(m_Context != nullptr);
	
	nvgEndFrame(m_Context);
}

void NanoVGRenderer::BeginPath()
{
	assert(m_Context != nullptr);
	
	nvgBeginPath(m_Context);
}

void NanoVGRenderer::MoveTo(float x, float y)
{
	assert(m_Context != nullptr);
	
	nvgMoveTo(m_Context, x, y);
}

void NanoVGRenderer::LineTo(float x, float y)
{
	assert(m_Context != nullptr);
	
	nvgLineTo(m_Context, x, y);
}

void NanoVGRenderer::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	assert(m_Context != nullptr);
	
	nvgBezierTo(m_Context, c1x, c1y, c2x, c2y, x, y);
}

void NanoVGRenderer::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	assert(m_Context != nullptr);
	
	nvgArcTo(m_Context, x1, y1, x2, y2, radius);
}

void NanoVGRenderer::Rect(float x, float y, float w, float h)
{
	assert(m_Context != nullptr);
	
	nvgRect(m_Context, x, y, w, h);
}

void NanoVGRenderer::RoundedRect(float x, float y, float w, float h, float r)
{
	assert(m_Context != nullptr);
	
	nvgRoundedRect(m_Context, x, y, w, h, r);
}

void NanoVGRenderer::Circle(float cx, float cy, float radius)
{
	assert(m_Context != nullptr);
	
	nvgCircle(m_Context, cx, cy, radius);
}

void NanoVGRenderer::ClosePath()
{
	assert(m_Context != nullptr);

	nvgClosePath(m_Context);
}

void NanoVGRenderer::FillConvexPath(uint32_t col, bool aa)
{
	BX_UNUSED(aa);
	assert(m_Context != nullptr);

	nvgFillColor(m_Context, nvgRGBA32(col));
	nvgFill(m_Context);
}

void NanoVGRenderer::FillConvexPath(GradientHandle handle, bool aa)
{
	BX_UNUSED(aa);
	assert(m_Context != nullptr);
	assert(handle.idx < MAX_PAINT_HANDLES);

	NVGpaint* paint = &m_Paints[handle.idx];
	nvgFillPaint(m_Context, *paint);
	nvgFill(m_Context);
}

void NanoVGRenderer::FillConvexPath(ImagePatternHandle handle, bool aa)
{
	BX_UNUSED(aa);
	assert(m_Context != nullptr);
	assert(handle.idx < MAX_PAINT_HANDLES);

	NVGpaint* paint = &m_Paints[handle.idx];
	nvgFillPaint(m_Context, *paint);
	nvgFill(m_Context);
}

void NanoVGRenderer::FillConcavePath(Color col, bool aa)
{
	BX_UNUSED(aa);
	assert(m_Context != nullptr);

	nvgFillColor(m_Context, nvgRGBA32(col));
	nvgFill(m_Context);
}

void NanoVGRenderer::StrokePath(uint32_t col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	BX_UNUSED(aa);
	assert(m_Context != nullptr);

#if 1
	nvgLineJoin(m_Context, NVG_MITER);
	nvgLineCap(m_Context, lineCap == LineCap::Butt ? NVG_BUTT : NVG_SQUARE);
#else
	nvgLineJoin(m_Context, lineJoin == LineJoin::Miter ? NVG_MITER : (lineJoin == LineJoin::Round ? NVG_ROUND : NVG_BEVEL));
	nvgLineCap(m_Context, lineCap == LineCap::Butt ? NVG_BUTT : (lineCap == LineCap::Round ? NVG_ROUND : NVG_SQUARE));
#endif
	nvgStrokeColor(m_Context, nvgRGBA32(col));
	nvgStrokeWidth(m_Context, width);
	nvgStroke(m_Context);
}

void NanoVGRenderer::PushState()
{
	nvgSave(m_Context);
}

void NanoVGRenderer::PopState()
{
	nvgRestore(m_Context);
}

void NanoVGRenderer::ResetScissor()
{
	nvgResetScissor(m_Context);
}

void NanoVGRenderer::Scissor(float x, float y, float w, float h)
{
	nvgScissor(m_Context, x, y, w, h);
}

bool NanoVGRenderer::IntersectScissor(float x, float y, float w, float h)
{
	return nvgIntersectScissor(m_Context, x, y, w, h);
}

void NanoVGRenderer::LoadIdentity()
{
	nvgResetTransform(m_Context);
}

void NanoVGRenderer::Scale(float x, float y)
{
	nvgScale(m_Context, x, y);
}

void NanoVGRenderer::Translate(float x, float y)
{
	nvgTranslate(m_Context, x, y);
}

void NanoVGRenderer::Rotate(float ang_rad)
{
	nvgRotate(m_Context, ang_rad);
}

void NanoVGRenderer::SetGlobalAlpha(float alpha)
{
	nvgGlobalAlpha(m_Context, alpha);
}

void NanoVGRenderer::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	nvgFillColor(m_Context, nvgRGBA32(color));
	nvgText(m_Context, x, y, text, end);
}

void NanoVGRenderer::TextBox(const Font& font, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* text, const char* end)
{
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	nvgFillColor(m_Context, nvgRGBA32(color));
	nvgTextBox(m_Context, x, y, breakWidth, text, end);
}

float NanoVGRenderer::CalcTextBounds(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, float* bounds)
{
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	return nvgTextBounds(m_Context, x, y, text, end, bounds);
}

void NanoVGRenderer::CalcTextBoxBounds(const Font& font, uint32_t alignment, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	nvgTextBoxBounds(m_Context, x, y, breakWidth, text, end, bounds, flags);
}

float NanoVGRenderer::GetTextLineHeight(const Font& font, uint32_t alignment)
{
	float lh;
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	nvgTextMetrics(m_Context, nullptr, nullptr, &lh);
	return lh;
}

int NanoVGRenderer::TextBreakLines(const Font& font, uint32_t alignment, const char* text, const char* end, float breakWidth, TextRow* rows, int numRows, uint32_t flags)
{
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	return nvgTextBreakLines(m_Context, text, end, breakWidth, (NVGtextRow*)rows, numRows, flags);
}

int NanoVGRenderer::TextGlyphPositions(const Font& font, uint32_t alignment, float x, float y, const char* text, const char* end, GlyphPosition* glyphs, int maxGlyphs)
{
	nvgFontFaceId(m_Context, font.m_Handle.idx);
	nvgFontSize(m_Context, font.m_Size);
	nvgTextAlign(m_Context, alignment);
	return nvgTextGlyphPositions(m_Context, x, y, text, end, (NVGglyphPosition*)glyphs, maxGlyphs);
}

ImagePatternHandle NanoVGRenderer::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	uint32_t paintID = m_NextPaintID;
	NVGpaint* paint = &m_Paints[paintID];
	*paint = nvgImagePattern(m_Context, cx, cy, w, h, angle, (int)image.idx, alpha);

	m_NextPaintID = (m_NextPaintID + 1) % MAX_PAINT_HANDLES;

	return { (uint16_t)paintID };
}

GradientHandle NanoVGRenderer::LinearGradient(float sx, float sy, float ex, float ey, vg::Color icol, vg::Color ocol)
{
	uint32_t paintID = m_NextPaintID;
	NVGpaint* paint = &m_Paints[paintID];

	*paint = nvgLinearGradient(m_Context, sx, sy, ex, ey, nvgRGBA32(icol), nvgRGBA32(ocol));

	m_NextPaintID = (m_NextPaintID + 1) % MAX_PAINT_HANDLES;

	return { (uint16_t)paintID };
}

GradientHandle NanoVGRenderer::BoxGradient(float x, float y, float w, float h, float r, float f, vg::Color icol, vg::Color ocol)
{
	uint32_t paintID = m_NextPaintID;
	NVGpaint* paint = &m_Paints[paintID];

	*paint = nvgBoxGradient(m_Context, x, y, w, h, r, f, nvgRGBA32(icol), nvgRGBA32(ocol));

	m_NextPaintID = (m_NextPaintID + 1) % MAX_PAINT_HANDLES;

	return { (uint16_t)paintID };
}

GradientHandle NanoVGRenderer::RadialGradient(float cx, float cy, float inr, float outr, vg::Color icol, vg::Color ocol)
{
	uint32_t paintID = m_NextPaintID;
	NVGpaint* paint = &m_Paints[paintID];

	*paint = nvgRadialGradient(m_Context, cx, cy, inr, outr, nvgRGBA32(icol), nvgRGBA32(ocol));

	m_NextPaintID = (m_NextPaintID + 1) % MAX_PAINT_HANDLES;

	return { (uint16_t)paintID };
}

ImageHandle NanoVGRenderer::CreateImageRGBA(int w, int h, uint32_t imageFlags, const uint8_t* data)
{
	BX_UNUSED(imageFlags);
	int imgID = nvgCreateImageRGBA(m_Context, w, h, 0, data);
	return { (uint16_t)imgID };
}

void NanoVGRenderer::UpdateImage(ImageHandle image, const uint8_t* data)
{
	nvgUpdateImage(m_Context, image.idx, data);
}

void NanoVGRenderer::GetImageSize(ImageHandle image, int* w, int* h)
{
	nvgImageSize(m_Context, image.idx, w, h);
}

void NanoVGRenderer::DeleteImage(ImageHandle image)
{
	nvgDeleteImage(m_Context, image.idx);
}

bool NanoVGRenderer::IsImageHandleValid(ImageHandle image)
{
	return image.idx != 0;
}

FontHandle NanoVGRenderer::LoadFontFromMemory(const char* name, const uint8_t* data, uint32_t size)
{
	uint8_t* fontData = (uint8_t*)BX_ALLOC(m_Allocator, size);
	memcpy(fontData, data, size);

	int fontHandle = nvgCreateFontMem(m_Context, name, fontData, size, 1);
	if (fontHandle == -1) {
		BX_FREE(m_Allocator, fontData);
		return BGFX_INVALID_HANDLE;
	}

	return{ (uint16_t)fontHandle };
}

Font NanoVGRenderer::CreateFontWithSize(const char* name, float size)
{
	Font f;
	f.m_Handle.idx = (uint16_t)nvgFindFont(m_Context, name);
	f.m_Size = size;
	return f;
}
}

