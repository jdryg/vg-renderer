BX_STATIC_ASSERT(sizeof(vg_color) == sizeof(vg::Color));
BX_STATIC_ASSERT(sizeof(vg_uv_t) == sizeof(vg::uv_t));
BX_STATIC_ASSERT(sizeof(vg_context_config) == sizeof(vg::ContextConfig));
BX_STATIC_ASSERT(sizeof(vg_stats) == sizeof(vg::Stats));
BX_STATIC_ASSERT(sizeof(vg_text_config) == sizeof(vg::TextConfig));
BX_STATIC_ASSERT(sizeof(vg_text_row) == sizeof(vg::TextRow));
BX_STATIC_ASSERT(sizeof(vg_glyph_position) == sizeof(vg::GlyphPosition));

namespace vg
{
class AllocatorC99 : public bx::AllocatorI
{
public:
	virtual ~AllocatorC99()
	{
	}

	virtual void* realloc(void* _ptr, size_t _size, size_t _align, const char* _file, uint32_t _line)
	{
		return m_Interface->realloc(m_Interface->m_Inst, _ptr, _size, _align, _file, _line);
	}

	vg_allocator_i* m_Interface;
};
} // namespace vg

VG_C_API vg_context* vg_createContext(vg_allocator_i* allocator, const vg_context_config* cfg)
{
	static vg::AllocatorC99 s_allocator;
	s_allocator.m_Interface = allocator;
	return (vg_context*)vg::createContext(&s_allocator, (const vg::ContextConfig*)cfg);
}

VG_C_API void vg_destroyContext(vg_context* ctx)
{
	vg::destroyContext((vg::Context*)ctx);
}

VG_C_API void vg_begin(vg_context* ctx, uint16_t viewID, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio)
{
	vg::begin((vg::Context*)ctx, viewID, canvasWidth, canvasHeight, devicePixelRatio);
}

VG_C_API void vg_end(vg_context* ctx)
{
	vg::end((vg::Context*)ctx);
}

VG_C_API void vg_frame(vg_context* ctx)
{
	vg::frame((vg::Context*)ctx);
}

VG_C_API const vg_stats* vg_getStats(vg_context* ctx)
{
	return (const vg_stats*)vg::getStats((vg::Context*)ctx);
}

VG_C_API void vg_beginPath(vg_context* ctx)
{
	vg::beginPath((vg::Context*)ctx);
}

VG_C_API void vg_moveTo(vg_context* ctx, float x, float y)
{
	vg::moveTo((vg::Context*)ctx, x, y);
}

VG_C_API void vg_lineTo(vg_context* ctx, float x, float y)
{
	vg::lineTo((vg::Context*)ctx, x, y);
}

VG_C_API void vg_cubicTo(vg_context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	vg::cubicTo((vg::Context*)ctx, c1x, c1y, c2x, c2y, x, y);
}

VG_C_API void vg_quadraticTo(vg_context* ctx, float cx, float cy, float x, float y)
{
	vg::quadraticTo((vg::Context*)ctx, cx, cy, x, y);
}

VG_C_API void vg_arcTo(vg_context* ctx, float x1, float y1, float x2, float y2, float r)
{
	vg::arcTo((vg::Context*)ctx, x1, y1, x2, y2, r);
}

VG_C_API void vg_arc(vg_context* ctx, float cx, float cy, float r, float a0, float a1, vg_winding dir)
{
	vg::arc((vg::Context*)ctx, cx, cy, r, a0, a1, (vg::Winding::Enum)dir);
}

VG_C_API void vg_rect(vg_context* ctx, float x, float y, float w, float h)
{
	vg::rect((vg::Context*)ctx, x, y, w, h);
}

VG_C_API void vg_roundedRect(vg_context* ctx, float x, float y, float w, float h, float r)
{
	vg::roundedRect((vg::Context*)ctx, x, y, w, h, r);
}

VG_C_API void vg_roundedRectVarying(vg_context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
	vg::roundedRectVarying((vg::Context*)ctx, x, y, w, h, rtl, rtr, rbr, rbl);
}

VG_C_API void vg_circle(vg_context* ctx, float cx, float cy, float radius)
{
	vg::circle((vg::Context*)ctx, cx, cy, radius);
}

VG_C_API void vg_ellipse(vg_context* ctx, float cx, float cy, float rx, float ry)
{
	vg::ellipse((vg::Context*)ctx, cx, cy, rx, ry);
}

VG_C_API void vg_polyline(vg_context* ctx, const float* coords, uint32_t numPoints)
{
	vg::polyline((vg::Context*)ctx, coords, numPoints);
}

VG_C_API void vg_closePath(vg_context* ctx)
{
	vg::closePath((vg::Context*)ctx);
}

VG_C_API void vg_fillPath_color(vg_context* ctx, vg_color color, uint32_t flags)
{
	vg::fillPath((vg::Context*)ctx, (vg::Color)color, flags);
}

VG_C_API void vg_fillPath_gradient(vg_context* ctx, vg_gradient_handle gradient, uint32_t flags)
{
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle = { gradient };
	vg::fillPath((vg::Context*)ctx, handle.cpp, flags);
}

VG_C_API void vg_fillPath_imagePattern(vg_context* ctx, vg_image_pattern_handle img, vg_color color, uint32_t flags)
{
	union { vg_image_pattern_handle c; vg::ImagePatternHandle cpp; } handle = { img };
	vg::fillPath((vg::Context*)ctx, handle.cpp, (vg::Color)color, flags);
}

VG_C_API void vg_strokePath_color(vg_context* ctx, vg_color color, float width, uint32_t flags)
{
	vg::strokePath((vg::Context*)ctx, (vg::Color)color, width, flags);
}

VG_C_API void vg_strokePath_gradient(vg_context* ctx, vg_gradient_handle gradient, float width, uint32_t flags)
{
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle = { gradient };
	vg::strokePath((vg::Context*)ctx, handle.cpp, width, flags);
}

VG_C_API void vg_strokePath_imagePattern(vg_context* ctx, vg_image_pattern_handle img, vg_color color, float width, uint32_t flags)
{
	union { vg_image_pattern_handle c; vg::ImagePatternHandle cpp; } handle = { img };
	vg::strokePath((vg::Context*)ctx, handle.cpp, (vg::Color)color, width, flags);
}

VG_C_API void vg_beginClip(vg_context* ctx, vg_clip_rule rule)
{
	vg::beginClip((vg::Context*)ctx, (vg::ClipRule::Enum)rule);
}

VG_C_API void vg_endClip(vg_context* ctx)
{
	vg::endClip((vg::Context*)ctx);
}

VG_C_API void vg_resetClip(vg_context* ctx)
{
	vg::resetClip((vg::Context*)ctx);
}

VG_C_API vg_gradient_handle vg_createLinearGradient(vg_context* ctx, float sx, float sy, float ex, float ey, vg_color icol, vg_color ocol)
{
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createLinearGradient((vg::Context*)ctx, sx, sy, ex, ey, (vg::Color)icol, (vg::Color)ocol);
	return handle_ret.c;
}

VG_C_API vg_gradient_handle vg_createBoxGradient(vg_context* ctx, float x, float y, float w, float h, float r, float f, vg_color icol, vg_color ocol)
{
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createBoxGradient((vg::Context*)ctx, x, y, w, h, r, f, (vg::Color)icol, (vg::Color)ocol);
	return handle_ret.c;
}

VG_C_API vg_gradient_handle vg_createRadialGradient(vg_context* ctx, float cx, float cy, float inr, float outr, vg_color icol, vg_color ocol)
{
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createRadialGradient((vg::Context*)ctx, cx, cy, inr, outr, (vg::Color)icol, (vg::Color)ocol);
	return handle_ret.c;
}

VG_C_API vg_image_pattern_handle vg_createImagePattern(vg_context* ctx, float cx, float cy, float w, float h, float angle, vg_image_handle image)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } handle = { image };
	union { vg_image_pattern_handle c; vg::ImagePatternHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createImagePattern((vg::Context*)ctx, cx, cy, w, h, angle, handle.cpp);
	return handle_ret.c;
}

VG_C_API void vg_setGlobalAlpha(vg_context* ctx, float alpha)
{
	vg::setGlobalAlpha((vg::Context*)ctx, alpha);
}

VG_C_API void vg_pushState(vg_context* ctx)
{
	vg::pushState((vg::Context*)ctx);
}

VG_C_API void vg_popState(vg_context* ctx)
{
	vg::popState((vg::Context*)ctx);
}

VG_C_API void vg_resetScissor(vg_context* ctx)
{
	vg::resetScissor((vg::Context*)ctx);
}

VG_C_API void vg_setScissor(vg_context* ctx, float x, float y, float w, float h)
{
	vg::setScissor((vg::Context*)ctx, x, y, w, h);
}

VG_C_API bool vg_intersectScissor(vg_context* ctx, float x, float y, float w, float h)
{
	return vg::intersectScissor((vg::Context*)ctx, x, y, w, h);
}

VG_C_API void vg_transformIdentity(vg_context* ctx)
{
	vg::transformIdentity((vg::Context*)ctx);
}

VG_C_API void vg_transformScale(vg_context* ctx, float x, float y)
{
	vg::transformScale((vg::Context*)ctx, x, y);
}

VG_C_API void vg_transformTranslate(vg_context* ctx, float x, float y)
{
	vg::transformTranslate((vg::Context*)ctx, x, y);
}

VG_C_API void vg_transformRotate(vg_context* ctx, float ang_rad)
{
	vg::transformRotate((vg::Context*)ctx, ang_rad);
}

VG_C_API void vg_transformMult(vg_context* ctx, const float* mtx, vg_transform_order order)
{
	vg::transformMult((vg::Context*)ctx, mtx, (vg::TransformOrder::Enum)order);
}

VG_C_API void vg_setViewBox(vg_context* ctx, float x, float y, float w, float h)
{
	vg::setViewBox((vg::Context*)ctx, x, y, w, h);
}

VG_C_API void vg_getTransform(vg_context* ctx, float* mtx)
{
	vg::getTransform((vg::Context*)ctx, mtx);
}

VG_C_API void vg_getScissor(vg_context* ctx, float* rect)
{
	vg::getScissor((vg::Context*)ctx, rect);
}

VG_C_API vg_font_handle vg_createFont(vg_context* ctx, const char* name, uint8_t* data, uint32_t size, uint32_t flags)
{
	union { vg_font_handle c; vg::FontHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createFont((vg::Context*)ctx, name, data, size, flags);
	return handle_ret.c;
}

VG_C_API vg_font_handle vg_getFontByName(vg_context* ctx, const char* name)
{
	union { vg_font_handle c; vg::FontHandle cpp; } handle_ret;
	handle_ret.cpp = vg::getFontByName((vg::Context*)ctx, name);
	return handle_ret.c;
}

VG_C_API bool vg_setFallbackFont(vg_context* ctx, vg_font_handle base, vg_font_handle fallback)
{
	union { vg_font_handle c; vg::FontHandle cpp; } baseHandle = { base }, fallbackHandle = { fallback };
	return vg::setFallbackFont((vg::Context*)ctx, baseHandle.cpp, fallbackHandle.cpp);
}

VG_C_API void vg_text(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* str, const char* end)
{
	vg::text((vg::Context*)ctx, *(vg::TextConfig*)cfg, x, y, str, end);
}

VG_C_API void vg_textBox(vg_context* ctx, const vg_text_config* cfg, float x, float y, float breakWidth, const char* text, const char* end, uint32_t textboxFlags)
{
	vg::textBox((vg::Context*)ctx, *(vg::TextConfig*)cfg, x, y, breakWidth, text, end, textboxFlags);
}

VG_C_API float vg_measureText(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* str, const char* end, float* bounds)
{
	return vg::measureText((vg::Context*)ctx, *(vg::TextConfig*)cfg, x, y, str, end, bounds);
}

VG_C_API void vg_measureTextBox(vg_context* ctx, const vg_text_config* cfg, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags)
{
	vg::measureTextBox((vg::Context*)ctx, *(vg::TextConfig*)cfg, x, y, breakWidth, text, end, bounds, flags);
}

VG_C_API float vg_getTextLineHeight(vg_context* ctx, const vg_text_config* cfg)
{
	return vg::getTextLineHeight((vg::Context*)ctx, *(vg::TextConfig*)cfg);
}

VG_C_API int32_t vg_textBreakLines(vg_context* ctx, const vg_text_config* cfg, const char* str, const char* end, float breakRowWidth, vg_text_row* rows, int32_t maxRows, uint32_t flags)
{
	return vg::textBreakLines((vg::Context*)ctx, *(vg::TextConfig*)cfg, str, end, breakRowWidth, (vg::TextRow*)rows, maxRows, flags);
}

VG_C_API int32_t vg_textGlyphPositions(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* text, const char* end, vg_glyph_position* positions, int32_t maxPositions)
{
	return vg::textGlyphPositions((vg::Context*)ctx, *(vg::TextConfig*)cfg, x, y, text, end, (vg::GlyphPosition*)positions, maxPositions);
}

VG_C_API void vg_indexedTriList(vg_context* ctx, const float* pos, const vg_uv_t* uv, uint32_t numVertices, const vg_color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, vg_image_handle img)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } imgHandle = { img };
	vg::indexedTriList((vg::Context*)ctx, pos, (vg::uv_t*)uv, numVertices, (vg::Color*)color, numColors, indices, numIndices, imgHandle.cpp);
}

VG_C_API bool vg_getImageSize(vg_context* ctx, vg_image_handle img, uint16_t* w, uint16_t* h)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } imgHandle = { img };
	return vg::getImageSize((vg::Context*)ctx, imgHandle.cpp, w, h);
}

VG_C_API vg_image_handle vg_createImage(vg_context* ctx, uint16_t w, uint16_t h, uint32_t flags, const uint8_t* data)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createImage((vg::Context*)ctx, w, h, flags, data);
	return handle_ret.c;
}

VG_C_API vg_image_handle vg_createImage_bgfx(vg_context* ctx, uint32_t flags, const bgfx_texture_handle_t* bgfxTextureHandle)
{
	union { bgfx_texture_handle_t c; bgfx::TextureHandle cpp; } texHandle = { *bgfxTextureHandle };
	union { vg_image_handle c; vg::ImageHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createImage((vg::Context*)ctx, flags, texHandle.cpp);
	return handle_ret.c;
}

VG_C_API bool vg_updateImage(vg_context* ctx, vg_image_handle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } handle = { image };
	return vg::updateImage((vg::Context*)ctx, handle.cpp, x, y, w, h, data);
}

VG_C_API bool vg_destroyImage(vg_context* ctx, vg_image_handle img)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } handle = { img };
	return vg::destroyImage((vg::Context*)ctx, handle.cpp);
}

VG_C_API bool vg_isImageValid(vg_context* ctx, vg_image_handle img)
{
	union { vg_image_handle c; vg::ImageHandle cpp; } handle = { img };
	return vg::isImageValid((vg::Context*)ctx, handle.cpp);
}

VG_C_API vg_command_list_handle vg_createCommandList(vg_context* ctx, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle_ret;
	handle_ret.cpp = vg::createCommandList((vg::Context*)ctx, flags);
	return handle_ret.c;
}

VG_C_API void vg_destroyCommandList(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::destroyCommandList((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_resetCommandList(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::resetCommandList((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_submitCommandList(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::submitCommandList((vg::Context*)ctx, handle.cpp);
}

#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
VG_C_API void vg_beginCommandList(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::beginCommandList((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_endCommandList(vg_context* ctx)
{
	vg::endCommandList((vg::Context*)ctx);
}
#endif

VG_C_API void vg_clBeginPath(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clBeginPath((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_clMoveTo(vg_context* ctx, vg_command_list_handle clh, float x, float y)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clMoveTo((vg::Context*)ctx, handle.cpp, x, y);
}

VG_C_API void vg_clLineTo(vg_context* ctx, vg_command_list_handle clh, float x, float y)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clLineTo((vg::Context*)ctx, handle.cpp, x, y);
}

VG_C_API void vg_clCubicTo(vg_context* ctx, vg_command_list_handle clh, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clCubicTo((vg::Context*)ctx, handle.cpp, c1x, c1y, c2x, c2y, x, y);
}

VG_C_API void vg_clQuadraticTo(vg_context* ctx, vg_command_list_handle clh, float cx, float cy, float x, float y)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clQuadraticTo((vg::Context*)ctx, handle.cpp, cx, cy, x, y);
}

VG_C_API void vg_clArcTo(vg_context* ctx, vg_command_list_handle clh, float x1, float y1, float x2, float y2, float r)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clArcTo((vg::Context*)ctx, handle.cpp, x1, y1, x2, y2, r);
}

VG_C_API void vg_clArc(vg_context* ctx, vg_command_list_handle clh, float cx, float cy, float r, float a0, float a1, vg_winding dir)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clArc((vg::Context*)ctx, handle.cpp, cx, cy, r, a0, a1, (vg::Winding::Enum)dir);
}

VG_C_API void vg_clRect(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clRect((vg::Context*)ctx, handle.cpp, x, y, w, h);
}

VG_C_API void vg_clRoundedRect(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h, float r)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clRoundedRect((vg::Context*)ctx, handle.cpp, x, y, w, h, r);
}

VG_C_API void vg_clRoundedRectVarying(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clRoundedRectVarying((vg::Context*)ctx, handle.cpp, x, y, w, h, rtl, rtr, rbr, rbl);
}

VG_C_API void vg_clCircle(vg_context* ctx, vg_command_list_handle clh, float cx, float cy, float radius)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clCircle((vg::Context*)ctx, handle.cpp, cx, cy, radius);
}

VG_C_API void vg_clEllipse(vg_context* ctx, vg_command_list_handle clh, float cx, float cy, float rx, float ry)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clEllipse((vg::Context*)ctx, handle.cpp, cx, cy, rx, ry);
}

VG_C_API void vg_clPolyline(vg_context* ctx, vg_command_list_handle clh, const float* coords, uint32_t numPoints)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clPolyline((vg::Context*)ctx, handle.cpp, coords, numPoints);
}

VG_C_API void vg_clClosePath(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clClosePath((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_clIndexedTriList(vg_context* ctx, vg_command_list_handle clh, const float* pos, const vg_uv_t* uv, uint32_t numVertices, const vg_color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, vg_image_handle img)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_image_handle c; vg::ImageHandle cpp; } imgHandle = { img };
	vg::clIndexedTriList((vg::Context*)ctx, handle.cpp, pos, (vg::uv_t*)uv, numVertices, (vg::Color*)color, numColors, indices, numIndices, imgHandle.cpp);
}

VG_C_API void vg_clFillPath_color(vg_context* ctx, vg_command_list_handle clh, vg_color color, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clFillPath((vg::Context*)ctx, handle.cpp, (vg::Color)color, flags);
}

VG_C_API void vg_clFillPath_gradient(vg_context* ctx, vg_command_list_handle clh, vg_gradient_handle gradient, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_gradient_handle c; vg::GradientHandle cpp; } gradientHandle = { gradient };
	vg::clFillPath((vg::Context*)ctx, handle.cpp, gradientHandle.cpp, flags);
}

VG_C_API void vg_clFillPath_imagePattern(vg_context* ctx, vg_command_list_handle clh, vg_image_pattern_handle img, vg_color color, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_image_pattern_handle c; vg::ImagePatternHandle cpp; } imgPatternHandle = { img };
	vg::clFillPath((vg::Context*)ctx, handle.cpp, imgPatternHandle.cpp, (vg::Color)color, flags);
}

VG_C_API void vg_clStrokePath_color(vg_context* ctx, vg_command_list_handle clh, vg_color color, float width, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clStrokePath((vg::Context*)ctx, handle.cpp, (vg::Color)color, width, flags);
}

VG_C_API void vg_clStrokePath_gradient(vg_context* ctx, vg_command_list_handle clh, vg_gradient_handle gradient, float width, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_gradient_handle c; vg::GradientHandle cpp; } gradientHandle = { gradient };
	vg::clStrokePath((vg::Context*)ctx, handle.cpp, gradientHandle.cpp, width, flags);
}

VG_C_API void vg_clStrokePath_imagePattern(vg_context* ctx, vg_command_list_handle clh, vg_image_pattern_handle img, vg_color color, float width, uint32_t flags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_image_pattern_handle c; vg::ImagePatternHandle cpp; } imgPatternHandle = { img };
	vg::clStrokePath((vg::Context*)ctx, handle.cpp, imgPatternHandle.cpp, (vg::Color)color, width, flags);
}

VG_C_API void vg_clBeginClip(vg_context* ctx, vg_command_list_handle clh, vg_clip_rule rule)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clBeginClip((vg::Context*)ctx, handle.cpp, (vg::ClipRule::Enum)rule);
}

VG_C_API void vg_clEndClip(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clEndClip((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_clResetClip(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clResetClip((vg::Context*)ctx, handle.cpp);
}

VG_C_API vg_gradient_handle vg_clCreateLinearGradient(vg_context* ctx, vg_command_list_handle clh, float sx, float sy, float ex, float ey, vg_color icol, vg_color ocol)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle_ret;
	handle_ret.cpp = vg::clCreateLinearGradient((vg::Context*)ctx, handle.cpp, sx, sy, ex, ey, (vg::Color)icol, (vg::Color)ocol);
	return handle_ret.c;
}

VG_C_API vg_gradient_handle vg_clCreateBoxGradient(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h, float r, float f, vg_color icol, vg_color ocol)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle_ret;
	handle_ret.cpp = vg::clCreateBoxGradient((vg::Context*)ctx, handle.cpp, x, y, w, h, r, f, (vg::Color)icol, (vg::Color)ocol);
	return handle_ret.c;
}

VG_C_API vg_gradient_handle vg_clCreateRadialGradient(vg_context* ctx, vg_command_list_handle clh, float cx, float cy, float inr, float outr, vg_color icol, vg_color ocol)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_gradient_handle c; vg::GradientHandle cpp; } handle_ret;
	handle_ret.cpp = vg::clCreateRadialGradient((vg::Context*)ctx, handle.cpp, cx, cy, inr, outr, (vg::Color)icol, (vg::Color)ocol);
	return handle_ret.c;
}

VG_C_API vg_image_pattern_handle vg_clCreateImagePattern(vg_context* ctx, vg_command_list_handle clh, float cx, float cy, float w, float h, float angle, vg_image_handle image)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	union { vg_image_pattern_handle c; vg::ImagePatternHandle cpp; } handle_ret;
	union { vg_image_handle c; vg::ImageHandle cpp; } imgHandle = { image };
	handle_ret.cpp = vg::clCreateImagePattern((vg::Context*)ctx, handle.cpp, cx, cy, w, h, angle, imgHandle.cpp);
	return handle_ret.c;
}

VG_C_API void vg_clPushState(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clPushState((vg::Context*) ctx, handle.cpp);
}

VG_C_API void vg_clPopState(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clPopState((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_clResetScissor(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clResetScissor((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_clSetScissor(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clSetScissor((vg::Context*)ctx, handle.cpp, x, y, w, h);
}

VG_C_API void vg_clIntersectScissor(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clIntersectScissor((vg::Context*)ctx, handle.cpp, x, y, w, h);
}

VG_C_API void vg_clTransformIdentity(vg_context* ctx, vg_command_list_handle clh)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clTransformIdentity((vg::Context*)ctx, handle.cpp);
}

VG_C_API void vg_clTransformScale(vg_context* ctx, vg_command_list_handle clh, float x, float y)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clTransformScale((vg::Context*)ctx, handle.cpp, x, y);
}

VG_C_API void vg_clTransformTranslate(vg_context* ctx, vg_command_list_handle clh, float x, float y)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clTransformTranslate((vg::Context*)ctx, handle.cpp, x, y);
}

VG_C_API void vg_clTransformRotate(vg_context* ctx, vg_command_list_handle clh, float ang_rad)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clTransformRotate((vg::Context*)ctx, handle.cpp, ang_rad);
}

VG_C_API void vg_clTransformMult(vg_context* ctx, vg_command_list_handle clh, const float* mtx, vg_transform_order order)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clTransformMult((vg::Context*)ctx, handle.cpp, mtx, (vg::TransformOrder::Enum)order);
}

VG_C_API void vg_clSetViewBox(vg_context* ctx, vg_command_list_handle clh, float x, float y, float w, float h)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clSetViewBox((vg::Context*)ctx, handle.cpp, x, y, w, h);
}

VG_C_API void vg_clText(vg_context* ctx, vg_command_list_handle clh, const vg_text_config* cfg, float x, float y, const char* str, const char* end)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clText((vg::Context*)ctx, handle.cpp, *(vg::TextConfig*)cfg, x, y, str, end);
}

VG_C_API void vg_clTextBox(vg_context* ctx, vg_command_list_handle clh, const vg_text_config* cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } handle = { clh };
	vg::clTextBox((vg::Context*)ctx, handle.cpp, *(vg::TextConfig*)cfg, x, y, breakWidth, str, end, textboxFlags);
}

VG_C_API void vg_clSubmitCommandList(vg_context* ctx, vg_command_list_handle parent, vg_command_list_handle child)
{
	union { vg_command_list_handle c; vg::CommandListHandle cpp; } parentHandle = { parent }, childHandle = { child };
	vg::clSubmitCommandList((vg::Context*)ctx, parentHandle.cpp, childHandle.cpp);
}

VG_C_API vg_api* vg_getAPI()
{
	static vg_api s_vg = {
		vg_createContext,
		vg_destroyContext,
		vg_begin,
		vg_end,
		vg_frame,
		vg_getStats,
		vg_beginPath,
		vg_moveTo,
		vg_lineTo,
		vg_cubicTo,
		vg_quadraticTo,
		vg_arcTo,
		vg_arc,
		vg_rect,
		vg_roundedRect,
		vg_roundedRectVarying,
		vg_circle,
		vg_ellipse,
		vg_polyline,
		vg_closePath,
		vg_fillPath_color,
		vg_fillPath_gradient,
		vg_fillPath_imagePattern,
		vg_strokePath_color,
		vg_strokePath_gradient,
		vg_strokePath_imagePattern,
		vg_beginClip,
		vg_endClip,
		vg_resetClip,
		vg_createLinearGradient,
		vg_createBoxGradient,
		vg_createRadialGradient,
		vg_createImagePattern,
		vg_setGlobalAlpha,
		vg_pushState,
		vg_popState,
		vg_resetScissor,
		vg_setScissor,
		vg_intersectScissor,
		vg_transformIdentity,
		vg_transformScale,
		vg_transformTranslate,
		vg_transformRotate,
		vg_transformMult,
		vg_setViewBox,
		vg_getTransform,
		vg_getScissor,
		vg_createFont,
		vg_getFontByName,
		vg_setFallbackFont,
		vg_text,
		vg_textBox,
		vg_measureText,
		vg_measureTextBox,
		vg_getTextLineHeight,
		vg_textBreakLines,
		vg_textGlyphPositions,
		vg_indexedTriList,
		vg_getImageSize,
		vg_createImage,
		vg_createImage_bgfx,
		vg_updateImage,
		vg_destroyImage,
		vg_isImageValid,
		vg_createCommandList,
		vg_destroyCommandList,
		vg_resetCommandList,
		vg_submitCommandList,
	#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
		vg_beginCommandList,
		vg_endCommandList,
	#endif
		vg_clBeginPath,
		vg_clMoveTo,
		vg_clLineTo,
		vg_clCubicTo,
		vg_clQuadraticTo,
		vg_clArcTo,
		vg_clArc,
		vg_clRect,
		vg_clRoundedRect,
		vg_clRoundedRectVarying,
		vg_clCircle,
		vg_clEllipse,
		vg_clPolyline,
		vg_clClosePath,
		vg_clIndexedTriList,
		vg_clFillPath_color,
		vg_clFillPath_gradient,
		vg_clFillPath_imagePattern,
		vg_clStrokePath_color,
		vg_clStrokePath_gradient,
		vg_clStrokePath_imagePattern,
		vg_clBeginClip,
		vg_clEndClip,
		vg_clResetClip,
		vg_clCreateLinearGradient,
		vg_clCreateBoxGradient,
		vg_clCreateRadialGradient,
		vg_clCreateImagePattern,
		vg_clPushState,
		vg_clPopState,
		vg_clResetScissor,
		vg_clSetScissor,
		vg_clIntersectScissor,
		vg_clTransformIdentity,
		vg_clTransformScale,
		vg_clTransformTranslate,
		vg_clTransformRotate,
		vg_clTransformMult,
		vg_clSetViewBox,
		vg_clText,
		vg_clTextBox,
		vg_clSubmitCommandList,
	};

	return &s_vg;
}
