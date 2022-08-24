#ifndef VG_VG_H
#define VG_VG_H

#include <stdint.h>
#include "../config.h"

typedef struct bgfx_texture_handle_s bgfx_texture_handle_t;

#if VG_CONFIG_UV_INT16
typedef int16_t vg_uv_t;
#else
typedef float vg_uv_t;
#endif

typedef uint32_t vg_color;

#if defined(__cplusplus)
#   define VG_C_API extern "C"
#else
#   define VG_C_API
#endif // defined(__cplusplus)

#define VG_COLOR_TRANSPARENT (vg_color)VG_COLOR32(0, 0, 0, 0)
#define VG_COLOR_BLACK       (vg_color)VG_COLOR32(0, 0, 0, 255)
#define VG_COLOR_RED         (vg_color)VG_COLOR32(255, 0, 0, 255)
#define VG_COLOR_GREEN       (vg_color)VG_COLOR32(0, 255, 0, 255)
#define VG_COLOR_BLUE        (vg_color)VG_COLOR32(0, 0, 255, 255)
#define VG_COLOR_WHITE       (vg_color)VG_COLOR32(255, 255, 255, 255)

typedef enum vg_text_align_hor
{
	VG_TEXT_ALIGN_HOR_LEFT   = 0,
	VG_TEXT_ALIGN_HOR_CENTER = 1,
	VG_TEXT_ALIGN_HOR_RIGHT  = 2
} vg_text_align_hor;

typedef enum vg_text_align_ver
{
	VG_TEXT_ALIGN_VER_TOP      = 0,
	VG_TEXT_ALIGN_VER_MIDDLE   = 1,
	VG_TEXT_ALIGN_VER_BASELINE = 2,
	VG_TEXT_ALIGN_VER_BOTTOM   = 3
} vg_text_align_ver;

typedef enum vg_text_align
{
	VG_TEXT_ALIGN_TOP_LEFT        = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_LEFT, VG_TEXT_ALIGN_VER_TOP),
	VG_TEXT_ALIGN_TOP_CENTER      = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_CENTER, VG_TEXT_ALIGN_VER_TOP),
	VG_TEXT_ALIGN_TOP_RIGHT       = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_RIGHT, VG_TEXT_ALIGN_VER_TOP),
	VG_TEXT_ALIGN_MIDDLE_LEFT     = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_LEFT, VG_TEXT_ALIGN_VER_MIDDLE),
	VG_TEXT_ALIGN_MIDDLE_CENTER   = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_CENTER, VG_TEXT_ALIGN_VER_MIDDLE),
	VG_TEXT_ALIGN_MIDDLE_RIGHT    = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_RIGHT, VG_TEXT_ALIGN_VER_MIDDLE),
	VG_TEXT_ALIGN_BASELINE_LEFT   = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_LEFT, VG_TEXT_ALIGN_VER_BASELINE),
	VG_TEXT_ALIGN_BASELINE_CENTER = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_CENTER, VG_TEXT_ALIGN_VER_BASELINE),
	VG_TEXT_ALIGN_BASELINE_RIGHT  = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_RIGHT, VG_TEXT_ALIGN_VER_BASELINE),
	VG_TEXT_ALIGN_BOTTOM_LEFT     = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_LEFT, VG_TEXT_ALIGN_VER_BOTTOM),
	VG_TEXT_ALIGN_BOTTOM_CENTER   = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_CENTER, VG_TEXT_ALIGN_VER_BOTTOM),
	VG_TEXT_ALIGN_BOTTOM_RIGHT    = VG_TEXT_ALIGN(VG_TEXT_ALIGN_HOR_RIGHT, VG_TEXT_ALIGN_VER_BOTTOM),
} vg_text_align;

typedef enum vg_line_cap
{
	VG_LINE_CAP_BUTT   = 0,
	VG_LINE_CAP_ROUND  = 1,
	VG_LINE_CAP_SQUARE = 2,
} vg_line_cap;

typedef enum vg_line_join
{
	VG_LINE_JOIN_MITER = 0,
	VG_LINE_JOIN_ROUND = 1,
	VG_LINE_JOIN_BEVEL = 2
} vg_line_join;

typedef enum vg_stroke_flags
{
	// w/o AA
	VG_STROKE_FLAGS_BUTT_MITER      = VG_STROKE_FLAGS(VG_LINE_CAP_BUTT, VG_LINE_JOIN_MITER, 0),
	VG_STROKE_FLAGS_BUTT_ROUND      = VG_STROKE_FLAGS(VG_LINE_CAP_BUTT, VG_LINE_JOIN_ROUND, 0),
	VG_STROKE_FLAGS_BUTT_BEVEL      = VG_STROKE_FLAGS(VG_LINE_CAP_BUTT, VG_LINE_JOIN_BEVEL, 0),
	VG_STROKE_FLAGS_ROUND_MITER     = VG_STROKE_FLAGS(VG_LINE_CAP_ROUND, VG_LINE_JOIN_MITER, 0),
	VG_STROKE_FLAGS_ROUND_ROUND     = VG_STROKE_FLAGS(VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND, 0),
	VG_STROKE_FLAGS_ROUND_BEVEL     = VG_STROKE_FLAGS(VG_LINE_CAP_ROUND, VG_LINE_JOIN_BEVEL, 0),
	VG_STROKE_FLAGS_SQUARE_MITER    = VG_STROKE_FLAGS(VG_LINE_CAP_SQUARE, VG_LINE_JOIN_MITER, 0),
	VG_STROKE_FLAGS_SQUARE_ROUND    = VG_STROKE_FLAGS(VG_LINE_CAP_SQUARE, VG_LINE_JOIN_ROUND, 0),
	VG_STROKE_FLAGS_SQUARE_BEVEL    = VG_STROKE_FLAGS(VG_LINE_CAP_SQUARE, VG_LINE_JOIN_BEVEL, 0),

	// w/ AA
	VG_STROKE_FLAGS_BUTT_MITER_AA   = VG_STROKE_FLAGS(VG_LINE_CAP_BUTT, VG_LINE_JOIN_MITER, 1),
	VG_STROKE_FLAGS_BUTT_ROUND_AA   = VG_STROKE_FLAGS(VG_LINE_CAP_BUTT, VG_LINE_JOIN_ROUND, 1),
	VG_STROKE_FLAGS_BUTT_BEVEL_AA   = VG_STROKE_FLAGS(VG_LINE_CAP_BUTT, VG_LINE_JOIN_BEVEL, 1),
	VG_STROKE_FLAGS_ROUND_MITER_AA  = VG_STROKE_FLAGS(VG_LINE_CAP_ROUND, VG_LINE_JOIN_MITER, 1),
	VG_STROKE_FLAGS_ROUND_ROUND_AA  = VG_STROKE_FLAGS(VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND, 1),
	VG_STROKE_FLAGS_ROUND_BEVEL_AA  = VG_STROKE_FLAGS(VG_LINE_CAP_ROUND, VG_LINE_JOIN_BEVEL, 1),
	VG_STROKE_FLAGS_SQUARE_MITER_AA = VG_STROKE_FLAGS(VG_LINE_CAP_SQUARE, VG_LINE_JOIN_MITER, 1),
	VG_STROKE_FLAGS_SQUARE_ROUND_AA = VG_STROKE_FLAGS(VG_LINE_CAP_SQUARE, VG_LINE_JOIN_ROUND, 1),
	VG_STROKE_FLAGS_SQUARE_BEVEL_AA = VG_STROKE_FLAGS(VG_LINE_CAP_SQUARE, VG_LINE_JOIN_BEVEL, 1),
	
	VG_STROKE_FLAGS_FIXED_WIDTH     = 1u << 5 // NOTE: Scale independent stroke width
} vg_stroke_flags;

typedef enum vg_path_type
{
	VG_PATH_TYPE_CONVEX = 0,
	VG_PATH_TYPE_CONCAVE = 1,
} vg_path_type;

typedef enum vg_fill_rule
{
	VG_FILL_RULE_NON_ZERO = 0,
	VG_FILL_RULE_EVEN_ODD = 1,
} vg_fill_rule;

typedef enum vg_fill_flags
{
	VG_FILL_FLAGS_CONVEX              = VG_FILL_FLAGS(VG_PATH_TYPE_CONVEX, VG_FILL_RULE_NON_ZERO, 0),
	VG_FILL_FLAGS_CONVEX_AA           = VG_FILL_FLAGS(VG_PATH_TYPE_CONVEX, VG_FILL_RULE_NON_ZERO, 1),
	VG_FILL_FLAGS_CONCAVE_NON_ZERO    = VG_FILL_FLAGS(VG_PATH_TYPE_CONCAVE, VG_FILL_RULE_NON_ZERO, 0),
	VG_FILL_FLAGS_CONCAVE_EVEN_ODD    = VG_FILL_FLAGS(VG_PATH_TYPE_CONCAVE, VG_FILL_RULE_EVEN_ODD, 0),
	VG_FILL_FLAGS_CONCAVE_NON_ZERO_AA = VG_FILL_FLAGS(VG_PATH_TYPE_CONCAVE, VG_FILL_RULE_NON_ZERO, 1),
	VG_FILL_FLAGS_CONCAVE_EVEN_ODD_AA = VG_FILL_FLAGS(VG_PATH_TYPE_CONCAVE, VG_FILL_RULE_EVEN_ODD, 1),
} vg_fill_flags;

typedef enum vg_winding
{
	VG_WINDING_CCW = 0,
	VG_WINDING_CW  = 1,
} vg_winding;

typedef enum vg_text_break_flags
{
	VG_TEXT_BREAK_FLAGS_NONE                 = 0,
	VG_TEXT_BREAK_FLAGS_KEEP_TRAILING_SPACES = 1u << 0,
} vg_text_break_flags;

typedef enum vg_image_flags
{
	VG_IMAGE_FILTER_NEAREST_UV = 1u << 0,
	VG_IMAGE_FILTER_NEAREST_W  = 1u << 1,
	VG_IMAGE_FILTER_LINEAR_UV  = 1u << 2,
	VG_IMAGE_FILTER_LINEAR_W   = 1u << 3,
	
	// Shortcuts
	VG_IMAGE_FILTER_NEAREST    = VG_IMAGE_FILTER_NEAREST_UV | VG_IMAGE_FILTER_NEAREST_W,
	VG_IMAGE_FILTER_BILINEAR   = VG_IMAGE_FILTER_LINEAR_UV | VG_IMAGE_FILTER_NEAREST_W,
	VG_IMAGE_FILTER_TRILINEAR  = VG_IMAGE_FILTER_LINEAR_UV | VG_IMAGE_FILTER_LINEAR_W
} vg_image_flags;

typedef enum vg_clip_rule
{
	VG_CLIP_RULE_IN  = 0, // fillRule = "nonzero"?
	VG_CLIP_RULE_OUT = 1, // fillRule = "evenodd"?
} vg_clip_rule;

typedef enum vg_transform_order
{
	VG_TRANSFORM_ORDER_PRE  = 0,
	VG_TRANSFORM_ORDER_POST = 1,
} vg_transform_order;

typedef struct vg_gradient_handle      { uint32_t idx; } vg_gradient_handle;
typedef struct vg_image_pattern_handle { uint32_t idx; } vg_image_pattern_handle;
typedef struct vg_image_handle         { uint16_t idx; } vg_image_handle;
typedef struct vg_font_handle          { uint16_t idx; } vg_font_handle;
typedef struct vg_command_list_handle  { uint16_t idx; } vg_command_list_handle;

#define VG_HANDLE_IS_VALID(h) ((h).idx != UINT16_MAX)

typedef struct vg_allocator_o vg_allocator_o;
typedef struct vg_allocator_i
{
	vg_allocator_o* m_Inst;

	void* (*realloc)(vg_allocator_o* inst, void* ptr, uint64_t sz, uint64_t align, const char* file, uint32_t line);
} vg_allocator_i;

typedef struct vg_context_config
{
	uint16_t m_MaxGradients;        // default: 64
	uint16_t m_MaxImagePatterns;    // default: 64
	uint16_t m_MaxFonts;            // default: 8
	uint16_t m_MaxStateStackSize;   // default: 32
	uint16_t m_MaxImages;           // default: 16
	uint16_t m_MaxCommandLists;     // default: 256
	uint32_t m_MaxVBVertices;       // default: 65536
	uint32_t m_FontAtlasImageFlags; // default: ImageFlags::Filter_Bilinear
	uint32_t m_MaxCommandListDepth; // default: 16
} vg_context_config;

typedef struct vg_stats
{
	uint32_t m_CmdListMemoryTotal;
	uint32_t m_CmdListMemoryUsed;
} vg_stats;

typedef struct vg_text_config
{
	float m_FontSize;
	float m_Blur;
	float m_Spacing;
	uint32_t m_Alignment;
	vg_color m_Color;
	vg_font_handle m_FontHandle;
} vg_text_config;

typedef struct vg_mesh
{
	const float* m_PosBuffer;
	const uint32_t* m_ColorBuffer;
	const uint16_t* m_IndexBuffer;
	uint32_t m_NumVertices;
	uint32_t m_NumIndices;
} vg_mesh;

// NOTE: The following 2 structs are identical to NanoVG because the rest of the code uses
// them a lot. Until I find a reason to replace them, these will do.
typedef struct vg_text_row
{
	const char* start;	// Pointer to the input text where the row starts.
	const char* end;	// Pointer to the input text where the row ends (one past the last character).
	const char* next;	// Pointer to the beginning of the next row.
	float width;		// Logical width of the row.
	float minx, maxx;	// Actual bounds of the row. Logical with and bounds can differ because of kerning and some parts over extending.
} vg_text_row;

typedef struct vg_glyph_position
{
	const char* str;	// Position of the glyph in the input string.
	float x;			// The x-coordinate of the logical glyph position.
	float minx, maxx;	// The bounds of the glyph shape.
} vg_glyph_position;

typedef enum vg_command_list_flags
{
	VG_COMMAND_LIST_FLAGS_NONE                  = 0,
	VG_COMMAND_LIST_FLAGS_CACHEABLE             = 1u << 0, // Cache the generated geometry in order to avoid retesselation every frame; uses extra memory
	VG_COMMAND_LIST_FLAGS_ALLOW_COMMAND_CULLING = 1u << 1, // If the scissor rect ends up being zero-sized, don't execute fill/stroke commands.
} vg_command_list_flags;

typedef enum vg_font_flags
{
	VG_FONT_FLAGS_NONE           = 0,
	VG_FONT_FLAGS_DONT_COPY_DATA = 1u << 0, // The calling code will keep the font data alive for as long as the Context is alive so there's no need to copy the data internally.
} vg_font_flags;

typedef struct vg_context vg_context;

VG_C_API vg_context* vg_createContext(vg_allocator_i* allocator, const vg_context_config* cfg);
VG_C_API void vg_destroyContext(vg_context* ctx);

VG_C_API void vg_begin(vg_context* ctx, uint16_t viewID, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio);
VG_C_API void vg_end(vg_context* ctx);
VG_C_API void vg_frame(vg_context* ctx);
VG_C_API const vg_stats* vg_getStats(vg_context* ctx);

VG_C_API void vg_beginPath(vg_context* ctx);
VG_C_API void vg_moveTo(vg_context* ctx, float x, float y);
VG_C_API void vg_lineTo(vg_context* ctx, float x, float y);
VG_C_API void vg_cubicTo(vg_context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
VG_C_API void vg_quadraticTo(vg_context* ctx, float cx, float cy, float x, float y);
VG_C_API void vg_arcTo(vg_context* ctx, float x1, float y1, float x2, float y2, float r);
VG_C_API void vg_arc(vg_context* ctx, float cx, float cy, float r, float a0, float a1, vg_winding dir);
VG_C_API void vg_rect(vg_context* ctx, float x, float y, float w, float h);
VG_C_API void vg_roundedRect(vg_context* ctx, float x, float y, float w, float h, float r);
VG_C_API void vg_roundedRectVarying(vg_context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
VG_C_API void vg_circle(vg_context* ctx, float cx, float cy, float radius);
VG_C_API void vg_ellipse(vg_context* ctx, float cx, float cy, float rx, float ry);
VG_C_API void vg_polyline(vg_context* ctx, const float* coords, uint32_t numPoints);
VG_C_API void vg_closePath(vg_context* ctx);
VG_C_API void vg_fillPath_color(vg_context* ctx, vg_color color, uint32_t flags);
VG_C_API void vg_fillPath_gradient(vg_context* ctx, vg_gradient_handle gradient, uint32_t flags);
VG_C_API void vg_fillPath_imagePattern(vg_context* ctx, vg_image_pattern_handle img, vg_color color, uint32_t flags);
VG_C_API void vg_strokePath_color(vg_context* ctx, vg_color color, float width, uint32_t flags);
VG_C_API void vg_strokePath_gradient(vg_context* ctx, vg_gradient_handle gradient, float width, uint32_t flags);
VG_C_API void vg_strokePath_imagePattern(vg_context* ctx, vg_image_pattern_handle img, vg_color color, float width, uint32_t flags);
VG_C_API void vg_beginClip(vg_context* ctx, vg_clip_rule rule);
VG_C_API void vg_endClip(vg_context* ctx);
VG_C_API void vg_resetClip(vg_context* ctx);

VG_C_API vg_gradient_handle vg_createLinearGradient(vg_context* ctx, float sx, float sy, float ex, float ey, vg_color icol, vg_color ocol);
VG_C_API vg_gradient_handle vg_createBoxGradient(vg_context* ctx, float x, float y, float w, float h, float r, float f, vg_color icol, vg_color ocol);
VG_C_API vg_gradient_handle vg_createRadialGradient(vg_context* ctx, float cx, float cy, float inr, float outr, vg_color icol, vg_color ocol);
VG_C_API vg_image_pattern_handle vg_createImagePattern(vg_context* ctx, float cx, float cy, float w, float h, float angle, vg_image_handle image);

VG_C_API void vg_setGlobalAlpha(vg_context* ctx, float alpha);
VG_C_API void vg_pushState(vg_context* ctx);
VG_C_API void vg_popState(vg_context* ctx);
VG_C_API void vg_resetScissor(vg_context* ctx);
VG_C_API void vg_setScissor(vg_context* ctx, float x, float y, float w, float h);
VG_C_API bool vg_intersectScissor(vg_context* ctx, float x, float y, float w, float h);
VG_C_API void vg_transformIdentity(vg_context* ctx);
VG_C_API void vg_transformScale(vg_context* ctx, float x, float y);
VG_C_API void vg_transformTranslate(vg_context* ctx, float x, float y);
VG_C_API void vg_transformRotate(vg_context* ctx, float ang_rad);
VG_C_API void vg_transformMult(vg_context* ctx, const float* mtx, vg_transform_order order);
VG_C_API void vg_setViewBox(vg_context* ctx, float x, float y, float w, float h);

VG_C_API void vg_getTransform(vg_context* ctx, float* mtx);
VG_C_API void vg_getScissor(vg_context* ctx, float* rect);

VG_C_API vg_font_handle vg_createFont(vg_context* ctx, const char* name, uint8_t* data, uint32_t size, uint32_t flags);
VG_C_API vg_font_handle vg_getFontByName(vg_context* ctx, const char* name);
VG_C_API bool vg_setFallbackFont(vg_context* ctx, vg_font_handle base, vg_font_handle fallback);
VG_C_API void vg_text(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* str, const char* end);
VG_C_API void vg_textBox(vg_context* ctx, const vg_text_config* cfg, float x, float y, float breakWidth, const char* text, const char* end, uint32_t textboxFlags);
VG_C_API float vg_measureText(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* str, const char* end, float* bounds);
VG_C_API void vg_measureTextBox(vg_context* ctx, const vg_text_config* cfg, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags);
VG_C_API float vg_getTextLineHeight(vg_context* ctx, const vg_text_config* cfg);
VG_C_API int32_t vg_textBreakLines(vg_context* ctx, const vg_text_config* cfg, const char* str, const char* end, float breakRowWidth, vg_text_row* rows, int32_t maxRows, uint32_t flags);
VG_C_API int32_t vg_textGlyphPositions(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* text, const char* end, vg_glyph_position* positions, int32_t maxPositions);

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
VG_C_API void vg_indexedTriList(vg_context* ctx, const float* pos, const vg_uv_t* uv, uint32_t numVertices, const vg_color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, vg_image_handle img);

VG_C_API bool vg_getImageSize(vg_context* ctx, vg_image_handle handle, uint16_t* w, uint16_t* h);
VG_C_API vg_image_handle vg_createImage(vg_context* ctx, uint16_t w, uint16_t h, uint32_t flags, const uint8_t* data);
VG_C_API vg_image_handle vg_createImage_bgfx(vg_context* ctx, uint32_t flags, const bgfx_texture_handle_t* bgfxTextureHandle);
VG_C_API bool vg_updateImage(vg_context* ctx, vg_image_handle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
VG_C_API bool vg_destroyImage(vg_context* ctx, vg_image_handle img);
VG_C_API bool vg_isImageValid(vg_context* ctx, vg_image_handle img);

// Command lists
VG_C_API vg_command_list_handle vg_createCommandList(vg_context* ctx, uint32_t flags);
VG_C_API void vg_destroyCommandList(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_resetCommandList(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_submitCommandList(vg_context* ctx, vg_command_list_handle handle);
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
VG_C_API void vg_beginCommandList(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_endCommandList(vg_context* ctx);
#endif

VG_C_API void vg_clBeginPath(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clMoveTo(vg_context* ctx, vg_command_list_handle handle, float x, float y);
VG_C_API void vg_clLineTo(vg_context* ctx, vg_command_list_handle handle, float x, float y);
VG_C_API void vg_clCubicTo(vg_context* ctx, vg_command_list_handle handle, float c1x, float c1y, float c2x, float c2y, float x, float y);
VG_C_API void vg_clQuadraticTo(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float x, float y);
VG_C_API void vg_clArcTo(vg_context* ctx, vg_command_list_handle handle, float x1, float y1, float x2, float y2, float r);
VG_C_API void vg_clArc(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float r, float a0, float a1, vg_winding dir);
VG_C_API void vg_clRect(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);
VG_C_API void vg_clRoundedRect(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h, float r);
VG_C_API void vg_clRoundedRectVarying(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
VG_C_API void vg_clCircle(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float radius);
VG_C_API void vg_clEllipse(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float rx, float ry);
VG_C_API void vg_clPolyline(vg_context* ctx, vg_command_list_handle handle, const float* coords, uint32_t numPoints);
VG_C_API void vg_clClosePath(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clIndexedTriList(vg_context* ctx, vg_command_list_handle handle, const float* pos, const vg_uv_t* uv, uint32_t numVertices, const vg_color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, vg_image_handle img);
VG_C_API void vg_clFillPath_color(vg_context* ctx, vg_command_list_handle handle, vg_color color, uint32_t flags);
VG_C_API void vg_clFillPath_gradient(vg_context* ctx, vg_command_list_handle handle, vg_gradient_handle gradient, uint32_t flags);
VG_C_API void vg_clFillPath_imagePattern(vg_context* ctx, vg_command_list_handle handle, vg_image_pattern_handle img, vg_color color, uint32_t flags);
VG_C_API void vg_clStrokePath_color(vg_context* ctx, vg_command_list_handle handle, vg_color color, float width, uint32_t flags);
VG_C_API void vg_clStrokePath_gradient(vg_context* ctx, vg_command_list_handle handle, vg_gradient_handle gradient, float width, uint32_t flags);
VG_C_API void vg_clStrokePath_imagePattern(vg_context* ctx, vg_command_list_handle handle, vg_image_pattern_handle img, vg_color color, float width, uint32_t flags);
VG_C_API void vg_clBeginClip(vg_context* ctx, vg_command_list_handle handle, vg_clip_rule rule);
VG_C_API void vg_clEndClip(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clResetClip(vg_context* ctx, vg_command_list_handle handle);

VG_C_API vg_gradient_handle vg_clCreateLinearGradient(vg_context* ctx, vg_command_list_handle handle, float sx, float sy, float ex, float ey, vg_color icol, vg_color ocol);
VG_C_API vg_gradient_handle vg_clCreateBoxGradient(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h, float r, float f, vg_color icol, vg_color ocol);
VG_C_API vg_gradient_handle vg_clCreateRadialGradient(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float inr, float outr, vg_color icol, vg_color ocol);
VG_C_API vg_image_pattern_handle vg_clCreateImagePattern(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float w, float h, float angle, vg_image_handle image);

VG_C_API void vg_clPushState(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clPopState(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clResetScissor(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clSetScissor(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);
VG_C_API void vg_clIntersectScissor(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);
VG_C_API void vg_clTransformIdentity(vg_context* ctx, vg_command_list_handle handle);
VG_C_API void vg_clTransformScale(vg_context* ctx, vg_command_list_handle handle, float x, float y);
VG_C_API void vg_clTransformTranslate(vg_context* ctx, vg_command_list_handle handle, float x, float y);
VG_C_API void vg_clTransformRotate(vg_context* ctx, vg_command_list_handle handle, float ang_rad);
VG_C_API void vg_clTransformMult(vg_context* ctx, vg_command_list_handle handle, const float* mtx, vg_transform_order order);
VG_C_API void vg_clSetViewBox(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);

VG_C_API void vg_clText(vg_context* ctx, vg_command_list_handle handle, const vg_text_config* cfg, float x, float y, const char* str, const char* end);
VG_C_API void vg_clTextBox(vg_context* ctx, vg_command_list_handle handle, const vg_text_config* cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);

VG_C_API void vg_clSubmitCommandList(vg_context* ctx, vg_command_list_handle parent, vg_command_list_handle child);

typedef struct vg_api
{
	vg_context* (*createContext)(vg_allocator_i* allocator, const vg_context_config* cfg);
	void (*destroyContext)(vg_context* ctx);

	void (*begin)(vg_context* ctx, uint16_t viewID, uint16_t canvasWidth, uint16_t canvasHeight, float devicePixelRatio);
	void (*end)(vg_context* ctx);
	void (*frame)(vg_context* ctx);
	const vg_stats* (*getStats)(vg_context* ctx);

	void (*beginPath)(vg_context* ctx);
	void (*moveTo)(vg_context* ctx, float x, float y);
	void (*lineTo)(vg_context* ctx, float x, float y);
	void (*cubicTo)(vg_context* ctx, float c1x, float c1y, float c2x, float c2y, float x, float y);
	void (*quadraticTo)(vg_context* ctx, float cx, float cy, float x, float y);
	void (*arcTo)(vg_context* ctx, float x1, float y1, float x2, float y2, float r);
	void (*arc)(vg_context* ctx, float cx, float cy, float r, float a0, float a1, vg_winding dir);
	void (*rect)(vg_context* ctx, float x, float y, float w, float h);
	void (*roundedRect)(vg_context* ctx, float x, float y, float w, float h, float r);
	void (*roundedRectVarying)(vg_context* ctx, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
	void (*circle)(vg_context* ctx, float cx, float cy, float radius);
	void (*ellipse)(vg_context* ctx, float cx, float cy, float rx, float ry);
	void (*polyline)(vg_context* ctx, const float* coords, uint32_t numPoints);
	void (*closePath)(vg_context* ctx);
	void (*fillPath_color)(vg_context* ctx, vg_color color, uint32_t flags);
	void (*fillPath_gradient)(vg_context* ctx, vg_gradient_handle gradient, uint32_t flags);
	void (*fillPath_imagePattern)(vg_context* ctx, vg_image_pattern_handle img, vg_color color, uint32_t flags);
	void (*strokePath_color)(vg_context* ctx, vg_color color, float width, uint32_t flags);
	void (*strokePath_gradient)(vg_context* ctx, vg_gradient_handle gradient, float width, uint32_t flags);
	void (*strokePath_imagePattern)(vg_context* ctx, vg_image_pattern_handle img, vg_color color, float width, uint32_t flags);
	void (*beginClip)(vg_context* ctx, vg_clip_rule rule);
	void (*endClip)(vg_context* ctx);
	void (*resetClip)(vg_context* ctx);

	vg_gradient_handle (*createLinearGradient)(vg_context* ctx, float sx, float sy, float ex, float ey, vg_color icol, vg_color ocol);
	vg_gradient_handle (*createBoxGradient)(vg_context* ctx, float x, float y, float w, float h, float r, float f, vg_color icol, vg_color ocol);
	vg_gradient_handle (*createRadialGradient)(vg_context* ctx, float cx, float cy, float inr, float outr, vg_color icol, vg_color ocol);
	vg_image_pattern_handle (*createImagePattern)(vg_context* ctx, float cx, float cy, float w, float h, float angle, vg_image_handle image);

	void (*setGlobalAlpha)(vg_context* ctx, float alpha);
	void (*pushState)(vg_context* ctx);
	void (*popState)(vg_context* ctx);
	void (*resetScissor)(vg_context* ctx);
	void (*setScissor)(vg_context* ctx, float x, float y, float w, float h);
	bool (*intersectScissor)(vg_context* ctx, float x, float y, float w, float h);
	void (*transformIdentity)(vg_context* ctx);
	void (*transformScale)(vg_context* ctx, float x, float y);
	void (*transformTranslate)(vg_context* ctx, float x, float y);
	void (*transformRotate)(vg_context* ctx, float ang_rad);
	void (*transformMult)(vg_context* ctx, const float* mtx, vg_transform_order order);
	void (*setViewBox)(vg_context* ctx, float x, float y, float w, float h);

	void (*getTransform)(vg_context* ctx, float* mtx);
	void (*getScissor)(vg_context* ctx, float* rect);

	vg_font_handle (*createFont)(vg_context* ctx, const char* name, uint8_t* data, uint32_t size, uint32_t flags);
	vg_font_handle(*getFontByName)(vg_context* ctx, const char* name);
	bool (*setFallbackFont)(vg_context* ctx, vg_font_handle base, vg_font_handle fallback);
	void (*text)(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* str, const char* end);
	void (*textBox)(vg_context* ctx, const vg_text_config* cfg, float x, float y, float breakWidth, const char* text, const char* end, uint32_t textboxFlags);
	float (*measureText)(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* str, const char* end, float* bounds);
	void (*measureTextBox)(vg_context* ctx, const vg_text_config* cfg, float x, float y, float breakWidth, const char* text, const char* end, float* bounds, uint32_t flags);
	float (*getTextLineHeight)(vg_context* ctx, const vg_text_config* cfg);
	int32_t (*textBreakLines)(vg_context* ctx, const vg_text_config* cfg, const char* str, const char* end, float breakRowWidth, vg_text_row* rows, int32_t maxRows, uint32_t flags);
	int32_t (*textGlyphPositions)(vg_context* ctx, const vg_text_config* cfg, float x, float y, const char* text, const char* end, vg_glyph_position* positions, int32_t maxPositions);

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
	void (*indexedTriList)(vg_context* ctx, const float* pos, const vg_uv_t* uv, uint32_t numVertices, const vg_color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, vg_image_handle img);

	bool (*getImageSize)(vg_context* ctx, vg_image_handle handle, uint16_t* w, uint16_t* h);
	vg_image_handle (*createImage)(vg_context* ctx, uint16_t w, uint16_t h, uint32_t flags, const uint8_t* data);
	vg_image_handle (*createImage_bgfx)(vg_context* ctx, uint32_t flags, const bgfx_texture_handle_t* bgfxTextureHandle);
	bool (*updateImage)(vg_context* ctx, vg_image_handle image, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* data);
	bool (*destroyImage)(vg_context* ctx, vg_image_handle img);
	bool (*isImageValid)(vg_context* ctx, vg_image_handle img);

	// Command lists
	vg_command_list_handle (*createCommandList)(vg_context* ctx, uint32_t flags);
	void (*destroyCommandList)(vg_context* ctx, vg_command_list_handle handle);
	void (*resetCommandList)(vg_context* ctx, vg_command_list_handle handle);
	void (*submitCommandList)(vg_context* ctx, vg_command_list_handle handle);
#if VG_CONFIG_COMMAND_LIST_BEGIN_END_API
	void (*beginCommandList)(vg_context* ctx, vg_command_list_handle handle);
	void (*endCommandList)(vg_context* ctx);
#endif

	void (*clBeginPath)(vg_context* ctx, vg_command_list_handle handle);
	void (*clMoveTo)(vg_context* ctx, vg_command_list_handle handle, float x, float y);
	void (*clLineTo)(vg_context* ctx, vg_command_list_handle handle, float x, float y);
	void (*clCubicTo)(vg_context* ctx, vg_command_list_handle handle, float c1x, float c1y, float c2x, float c2y, float x, float y);
	void (*clQuadraticTo)(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float x, float y);
	void (*clArcTo)(vg_context* ctx, vg_command_list_handle handle, float x1, float y1, float x2, float y2, float r);
	void (*clArc)(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float r, float a0, float a1, vg_winding dir);
	void (*clRect)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);
	void (*clRoundedRect)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h, float r);
	void (*clRoundedRectVarying)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h, float rtl, float rtr, float rbr, float rbl);
	void (*clCircle)(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float radius);
	void (*clEllipse)(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float rx, float ry);
	void (*clPolyline)(vg_context* ctx, vg_command_list_handle handle, const float* coords, uint32_t numPoints);
	void (*clClosePath)(vg_context* ctx, vg_command_list_handle handle);
	void (*clIndexedTriList)(vg_context* ctx, vg_command_list_handle handle, const float* pos, const vg_uv_t* uv, uint32_t numVertices, const vg_color* color, uint32_t numColors, const uint16_t* indices, uint32_t numIndices, vg_image_handle img);
	void (*clFillPath_color)(vg_context* ctx, vg_command_list_handle handle, vg_color color, uint32_t flags);
	void (*clFillPath_gradient)(vg_context* ctx, vg_command_list_handle handle, vg_gradient_handle gradient, uint32_t flags);
	void (*clFillPath_imagePattern)(vg_context* ctx, vg_command_list_handle handle, vg_image_pattern_handle img, vg_color color, uint32_t flags);
	void (*clStrokePath_color)(vg_context* ctx, vg_command_list_handle handle, vg_color color, float width, uint32_t flags);
	void (*clStrokePath_gradient)(vg_context* ctx, vg_command_list_handle handle, vg_gradient_handle gradient, float width, uint32_t flags);
	void (*clStrokePath_imagePattern)(vg_context* ctx, vg_command_list_handle handle, vg_image_pattern_handle img, vg_color color, float width, uint32_t flags);
	void (*clBeginClip)(vg_context* ctx, vg_command_list_handle handle, vg_clip_rule rule);
	void (*clEndClip)(vg_context* ctx, vg_command_list_handle handle);
	void (*clResetClip)(vg_context* ctx, vg_command_list_handle handle);

	vg_gradient_handle (*clCreateLinearGradient)(vg_context* ctx, vg_command_list_handle handle, float sx, float sy, float ex, float ey, vg_color icol, vg_color ocol);
	vg_gradient_handle (*clCreateBoxGradient)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h, float r, float f, vg_color icol, vg_color ocol);
	vg_gradient_handle (*clCreateRadialGradient)(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float inr, float outr, vg_color icol, vg_color ocol);
	vg_image_pattern_handle (*clCreateImagePattern)(vg_context* ctx, vg_command_list_handle handle, float cx, float cy, float w, float h, float angle, vg_image_handle image);

	void (*clPushState)(vg_context* ctx, vg_command_list_handle handle);
	void (*clPopState)(vg_context* ctx, vg_command_list_handle handle);
	void (*clResetScissor)(vg_context* ctx, vg_command_list_handle handle);
	void (*clSetScissor)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);
	void (*clIntersectScissor)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);
	void (*clTransformIdentity)(vg_context* ctx, vg_command_list_handle handle);
	void (*clTransformScale)(vg_context* ctx, vg_command_list_handle handle, float x, float y);
	void (*clTransformTranslate)(vg_context* ctx, vg_command_list_handle handle, float x, float y);
	void (*clTransformRotate)(vg_context* ctx, vg_command_list_handle handle, float ang_rad);
	void (*clTransformMult)(vg_context* ctx, vg_command_list_handle handle, const float* mtx, vg_transform_order order);
	void (*clSetViewBox)(vg_context* ctx, vg_command_list_handle handle, float x, float y, float w, float h);

	void (*clText)(vg_context* ctx, vg_command_list_handle handle, const vg_text_config* cfg, float x, float y, const char* str, const char* end);
	void (*clTextBox)(vg_context* ctx, vg_command_list_handle handle, const vg_text_config* cfg, float x, float y, float breakWidth, const char* str, const char* end, uint32_t textboxFlags);

	void (*clSubmitCommandList)(vg_context* ctx, vg_command_list_handle parent, vg_command_list_handle child);
} vg_api;

typedef vg_api* (*PFN_VG_GET_API)();

VG_C_API vg_api* vg_getAPI();

#endif // VG_VG_H
