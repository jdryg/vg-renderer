#ifndef VG_H
#	error "Must be included from vg/vg.h"
#endif

namespace vg
{
inline Color color4f(float r, float g, float b, float a)
{
	uint8_t rb = (uint8_t)(bx::clamp<float>(r, 0.0f, 1.0f) * 255.0f);
	uint8_t gb = (uint8_t)(bx::clamp<float>(g, 0.0f, 1.0f) * 255.0f);
	uint8_t bb = (uint8_t)(bx::clamp<float>(b, 0.0f, 1.0f) * 255.0f);
	uint8_t ab = (uint8_t)(bx::clamp<float>(a, 0.0f, 1.0f) * 255.0f);
	return VG_COLOR32(rb, gb, bb, ab);
}

inline Color color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return VG_COLOR32(r, g, b, a);
}

inline Color colorHSB(float hue, float sat, float brightness)
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

inline Color colorSetAlpha(Color c, uint8_t a)
{
	return (c & VG_COLOR_RGB_MASK) | ((uint32_t)a << VG_COLOR_ALPHA_SHIFT);
}

inline uint8_t colorGetAlpha(Color c) 
{
	return (uint8_t)((c >> VG_COLOR_ALPHA_SHIFT) & 0xFF); 
}

inline uint8_t colorGetRed(Color c) 
{ 
	return (uint8_t)((c >> VG_COLOR_RED_SHIFT) & 0xFF); 
}

inline uint8_t colorGetGreen(Color c) 
{
	return (uint8_t)((c >> VG_COLOR_GREEN_SHIFT) & 0xFF); 
}

inline uint8_t colorGetBlue(Color c)
{
	return (uint8_t)((c >> VG_COLOR_BLUE_SHIFT) & 0xFF); 
}

// Text helpers
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

inline void textBox(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, Color color, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
	textBox(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, color), x, y, breakWidth, str, end, textboxFlags);
}

inline float measureText(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, const char* str, const char* end, float* bounds)
{
	return measureText(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, Colors::Transparent), x, y, str, end, bounds);
}

inline void measureTextBox(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, float breakWidth, const char* str, const char* end, float* bounds, uint32_t flags)
{
	measureTextBox(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, Colors::Transparent), x, y, breakWidth, str, end, bounds, flags);
}

inline float getTextLineHeight(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment)
{
	return getTextLineHeight(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, Colors::Transparent));
}

inline int textBreakLines(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, const char* str, const char* end, float breakRowWidth, TextRow* rows, int maxRows, uint32_t flags)
{
	return textBreakLines(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, Colors::Transparent), str, end, breakRowWidth, rows, maxRows, flags);
}

inline int textGlyphPositions(Context* ctx, FontHandle fontHandle, float fontSize, uint32_t alignment, float x, float y, const char* str, const char* end, GlyphPosition* positions, int maxPositions)
{
	return textGlyphPositions(ctx, makeTextConfig(ctx, fontHandle, fontSize, alignment, Colors::Transparent), x, y, str, end, positions, maxPositions);
}

// Command list helpers
inline CommandListRef makeCommandListRef(Context* ctx, CommandListHandle handle)
{
	return { ctx, handle };
}

inline void clReset(CommandListRef& ref)
{
	resetCommandList(ref.m_Context, ref.m_Handle);
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

inline void clRoundedRectVarying(CommandListRef& ref, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
	clRoundedRectVarying(ref.m_Context, ref.m_Handle, x, y, w, h, rtl, rtr, rbr, rbl);
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

inline GradientHandle clCreateLinearGradient(CommandListRef& ref, float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	return clCreateLinearGradient(ref.m_Context, ref.m_Handle, sx, sy, ex, ey, icol, ocol);
}

inline GradientHandle clCreateBoxGradient(CommandListRef& ref, float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	return clCreateBoxGradient(ref.m_Context, ref.m_Handle, x, y, w, h, r, f, icol, ocol);
}

inline GradientHandle clCreateRadialGradient(CommandListRef& ref, float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	return clCreateRadialGradient(ref.m_Context, ref.m_Handle, cx, cy, inr, outr, icol, ocol);
}

inline ImagePatternHandle clCreateImagePattern(CommandListRef& ref, float cx, float cy, float w, float h, float angle, ImageHandle image)
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

inline void clTransformMult(CommandListRef& ref, const float* mtx, TransformOrder::Enum order)
{
	clTransformMult(ref.m_Context, ref.m_Handle, mtx, order);
}

inline void clSetViewBox(CommandListRef& ref, float x, float y, float w, float h)
{
	clSetViewBox(ref.m_Context, ref.m_Handle, x, y, w, h);
}

inline void clText(CommandListRef& ref, const TextConfig& cfg, float x, float y, const char* str, const char* end)
{
	clText(ref.m_Context, ref.m_Handle, cfg, x, y, str, end);
}

inline void clTextBox(CommandListRef& ref, const TextConfig& cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
	clTextBox(ref.m_Context, ref.m_Handle, cfg, x, y, breakWidth, str, end, textboxFlags);
}

inline void clSubmitCommandList(CommandListRef& ref, CommandListHandle child)
{
	clSubmitCommandList(ref.m_Context, ref.m_Handle, child);
}
}
