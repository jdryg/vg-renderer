// This code is heavily based on FontStash (c) 2009-2013 Mikko Mononen memon@inside.org
// Some parts of the original code have been removed and other have been added.

// TODO: bx::HandleAlloc for fonts so we are able to remove fonts and keep the same handles
#include "font_system.h"
#include "vg_util.h"
#include <bx/allocator.h>
#include <bx/string.h>

#define FS_CONFIG_LUT_SIZE           256
#define FS_CONFIG_MAX_FALLBACK_FONTS 8
#define FS_CONFIG_MAX_FONT_IMAGES    4
#define FS_CONFIG_SNAP_TO_GRID       0
#define FS_CONFIG_FONT_SIZE_EM       0
#define FS_CONFIG_TAB_SIZE           4.0f // * Space size

#if FS_CONFIG_SNAP_TO_GRID
#define FS_SNAP_COORD(coord) (float)((int32_t)((coord) + 0.5f))
#else
#define FS_SNAP_COORD(coord) (coord)
#endif

#define FS_MAKE_GLYPH_CODE(cp, size, blur) (((uint64_t)(cp)) | ((uint64_t)(size) << 32) | ((uint64_t)(blur) << 48))

namespace vg
{
struct AtlasNode
{
	uint16_t m_X;
	uint16_t m_Y;
	uint16_t m_Width;
	uint16_t _Reserved[1];
};

struct Atlas
{
	bx::AllocatorI* m_Allocator;
	AtlasNode* m_Nodes;
	uint32_t m_NumNodes;
	uint32_t m_NodeCapacity;
	uint16_t m_Width;
	uint16_t m_Height;
};

struct Glyph
{
	uint64_t m_GlyphCode;
	int32_t m_Next;
	uint16_t m_RectPos[2];  // { x, y }
	uint16_t m_RectSize[2]; // { w, h }
	int16_t m_XAdv, m_XOff, m_YOff;
};

struct Font
{
	uint8_t* m_Data;
	uint32_t m_DataSize;
	uint32_t m_Flags;
	void* m_BackendData;
	Glyph* m_Glyphs;
	uint32_t m_GlyphCapacity;
	uint32_t m_NumGlyphs;
	int32_t m_LUT[FS_CONFIG_LUT_SIZE];
	FontHandle m_Fallback[FS_CONFIG_MAX_FALLBACK_FONTS];
	uint32_t m_NumFallbacks;
	float m_Ascender;
	float m_Descender;
	float m_LineHeight;
	char m_Name[64];
};

struct TextBuffer
{
	uint8_t* m_Buffer;
	uint32_t m_Size;
	uint32_t m_Capacity;

	TextQuad* m_Quads;
	uint32_t* m_Codepoints;
	uint8_t* m_CodepointSize;
	int32_t* m_GlyphIndices;
	int32_t* m_KernAdv;
	FontHandle* m_GlyphFonts;
};

struct FontSystem
{
	bx::AllocatorI* m_Allocator;
	Atlas* m_Atlas;
	Font* m_Fonts;
	uint8_t* m_ImageData;
	FontSystemConfig m_Config;
	TextBuffer m_TextBuffer;
	ImageHandle m_FontImages[FS_CONFIG_MAX_FONT_IMAGES];
	uint32_t m_FontImageID;
	uv_t m_FontImageWhitePixelUV[2];
	uint32_t m_NumFonts;
	uint32_t m_FontCapacity;
	uint32_t m_AtlasID;
	uint16_t m_DirtyRect[4]; // { minx, miny, maxx, maxy }
};

static bool fsAddWhiteRect(FontSystem* fs, uint16_t rectWidth, uint16_t rectHeight);
static void fsInvalidateRect(FontSystem* fs, uint16_t minX, uint16_t minY, uint16_t maxX, uint16_t maxY);
static FontHandle fsAllocFont(FontSystem* fs);
static bool fsResetAtlas(FontSystem* fs, uint16_t width, uint16_t height);
static Atlas* fsCreateAtlas(bx::AllocatorI* allocator, uint16_t w, uint16_t h);
static void fsDestroyAtlas(Atlas* atlas);
static uint32_t fsAtlasAllocNode(Atlas* atlas);
static void fsAtlasSetNode(Atlas* atlas, uint32_t nodeID, uint16_t x, uint16_t y, uint16_t w);
static bool fsAtlasInsertNode(Atlas* atlas, uint32_t nodeID, uint16_t x, uint16_t y, uint16_t w);
static void fsAtlasRemoveNode(Atlas* atlas, uint32_t nodeID);
static bool fsAtlasAddRect(Atlas* atlas, uint16_t rectWidth, uint16_t rectHeight, uint16_t* rectX, uint16_t* rectY);
static uint32_t fsAtlasRectFits(Atlas* atlas, uint32_t nodeID, uint16_t rectWidth, uint16_t rectHeight);
static bool fsAtlasAddSkylineLevel(Atlas* atlas, uint32_t nodeID, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static void fsAtlasReset(Atlas* atlas, uint16_t w, uint16_t h);
static uint32_t decodeUTF8(uint32_t* state, uint32_t* codep, uint8_t byte);
static void fsUpdateWhitePixelUV(FontSystem* fs, vg::Context* ctx);
static uint32_t fsTextBuildMesh(FontSystem* fs, TextBuffer* tb, vg::Context* ctx, const vg::TextConfig& cfg, uint32_t flags, TextMesh* mesh);
static void fsTextBufferInit(TextBuffer* tb);
static void fsTextBufferShutdown(TextBuffer* tb, bx::AllocatorI* allocator);
static bool fsTextBufferReset(TextBuffer* tb, uint32_t capacity, bx::AllocatorI* allocator);
static bool fsTextBufferPushCodepoint(TextBuffer* tb, uint32_t codepoint, uint8_t codepointSize, bx::AllocatorI* allocator);
static float fsGetVertAlign(FontSystem* fs, const Font* font, uint32_t align, int16_t isize);
static Glyph* fsBakeGlyph(FontSystem* fs, Font* font, int32_t glyphIndex, uint32_t codepoint, int16_t isize, int16_t iblur, bool glyphBitmapOptional);
static Glyph* fsAllocGlyph(FontSystem* fs, Font* font);
static bool fsAllocTextAtlas(FontSystem* fs, Context* ctx);

static bool fsBackendInit(FontSystem* fs);
static void* fsBackendLoadFont(FontSystem* fs, uint8_t* data, uint32_t dataSize);
static void fsBackendFreeFont(FontSystem* fs, void* fontPtr);
static void fsBackendGetFontVMetrics(void* fontPtr, int32_t* ascent, int32_t* descent, int32_t* lineGap);
static float fsBackendGetPixelHeightScale(void* fontPtr, float size);
static int32_t fsBackendGetGlyphIndex(void* fontPtr, uint32_t codepoint);
static bool fsBackendBuildGlyphBitmap(void* fontPtr, int32_t glyph, float size, float scale, int32_t* advance, int32_t* lsb, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1);
static void fsBackendRenderGlyphBitmap(void* fontPtr, uint8_t* output, int32_t outWidth, int32_t outHeight, int32_t outStride, float scaleX, float scaleY, int glyph);
static int32_t fsBackendGetGlyphKernAdvance(void* fontPtr, int32_t glyph1, int32_t glyph2);

FontSystem* fsCreate(vg::Context* ctx, bx::AllocatorI* allocator, const FontSystemConfig* cfg)
{
	FontSystem* fs = (FontSystem*)bx::alloc(allocator, sizeof(FontSystem));
	if (!fs) {
		return nullptr;
	}

	bx::memSet(fs, 0, sizeof(FontSystem));
	fs->m_Allocator = allocator;
	bx::memCopy(&fs->m_Config, cfg, sizeof(FontSystemConfig));

	fsBackendInit(fs);

	fs->m_Atlas = fsCreateAtlas(allocator, cfg->m_AtlasWidth, cfg->m_AtlasHeight);
	if (!fs->m_Atlas) {
		fsDestroy(fs, ctx);
		return nullptr;
	}

	fs->m_AtlasID = 1;

	// Initialize image data
	fs->m_ImageData = (uint8_t*)bx::alloc(allocator, (size_t)cfg->m_AtlasWidth * (size_t)cfg->m_AtlasHeight);
	if (!fs->m_ImageData) {
		fsDestroy(fs, ctx);
		return nullptr;
	}
	bx::memSet(fs->m_ImageData, 0, (size_t)cfg->m_AtlasWidth * (size_t)cfg->m_AtlasHeight);

	// Initialize dirty rect (mark whole texture as dirty to 
	// force the renderer to update the texture)
	fs->m_DirtyRect[0] = 0;
	fs->m_DirtyRect[1] = 0;
	fs->m_DirtyRect[2] = cfg->m_AtlasWidth;
	fs->m_DirtyRect[3] = cfg->m_AtlasHeight;

	// Add white rect
	fsAddWhiteRect(fs, cfg->m_WhiteRectWidth, cfg->m_WhiteRectHeight);

	// Initialize images
	for (uint32_t i = 0; i < FS_CONFIG_MAX_FONT_IMAGES; ++i) {
		fs->m_FontImages[i] = VG_INVALID_HANDLE;
	}

	fs->m_FontImages[0] = createImage(ctx, (uint16_t)cfg->m_AtlasWidth, (uint16_t)cfg->m_AtlasHeight, cfg->m_FontAtlasImageFlags, nullptr);
	VG_CHECK(isValid(fs->m_FontImages[0]), "Failed to initialize font texture");

	fs->m_FontImageID = 0;

	fsUpdateWhitePixelUV(fs, ctx);
	fsTextBufferInit(&fs->m_TextBuffer);

	return fs;
}

void fsDestroy(FontSystem* fs, vg::Context* ctx)
{
	bx::AllocatorI* allocator = fs->m_Allocator;

	fsTextBufferShutdown(&fs->m_TextBuffer, allocator);

	for (uint32_t i = 0; i < FS_CONFIG_MAX_FONT_IMAGES; ++i) {
		destroyImage(ctx, fs->m_FontImages[i]);
	}

	const uint32_t numFonts = fs->m_NumFonts;
	for (uint32_t i = 0; i < numFonts; ++i) {
		Font* font = &fs->m_Fonts[i];
		fsBackendFreeFont(fs, font->m_BackendData);

		if ((font->m_Flags & FontFlags::DontCopyData) == 0) {
			bx::free(allocator, font->m_Data);
		}

		bx::free(allocator, font->m_Glyphs);
	}
	bx::free(allocator, fs->m_Fonts);

	bx::free(allocator, fs->m_ImageData);

	fsDestroyAtlas(fs->m_Atlas);

	bx::free(allocator, fs);
}

void fsFrame(FontSystem* fs, vg::Context* ctx)
{
	if (fs->m_FontImageID != 0) {
		vg::ImageHandle fontImage = fs->m_FontImages[fs->m_FontImageID];

		// delete images that smaller than current one
		if (vg::isValid(fontImage)) {
			uint16_t iw, ih;
			vg::getImageSize(ctx, fontImage, &iw, &ih);

			uint32_t j = 0;
			for (uint32_t i = 0; i < fs->m_FontImageID; i++) {
				if (vg::isValid(fs->m_FontImages[i])) {
					uint16_t nw, nh;
					vg::getImageSize(ctx, fs->m_FontImages[i], &nw, &nh);

					if (nw < iw || nh < ih) {
						vg::destroyImage(ctx, fs->m_FontImages[i]);
					} else {
						fs->m_FontImages[j++] = fs->m_FontImages[i];
					}
				}
			}

			// make current font image to first
			fs->m_FontImages[j++] = fs->m_FontImages[0];
			fs->m_FontImages[0] = fontImage;
			fs->m_FontImageID = 0;
			fsUpdateWhitePixelUV(fs, ctx);

			// clear all images after j
			for (int i = j; i < FS_CONFIG_MAX_FONT_IMAGES; i++) {
				fs->m_FontImages[i] = VG_INVALID_HANDLE;
			}
		}
	}
}

FontHandle fsAddFont(FontSystem* fs, const char* name, uint8_t* data, uint32_t dataSize, uint32_t fontFlags)
{
	uint8_t* fontData = nullptr;
	const bool copyData = (fontFlags & FontFlags::DontCopyData) == 0;
	if (copyData) {
		fontData = (uint8_t*)bx::alloc(fs->m_Allocator, dataSize);
		if (!fontData) {
			return VG_INVALID_HANDLE;
		}

		bx::memCopy(fontData, data, dataSize);
	} else {
		fontData = data;
	}

	const FontHandle handle = fsAllocFont(fs);
	if (!isValid(handle)) {
		return VG_INVALID_HANDLE;
	}

	Font* font = &fs->m_Fonts[handle.idx];
	bx::snprintf(font->m_Name, BX_COUNTOF(font->m_Name), "%s", name);

	// Read in the font data.
	font->m_DataSize = dataSize;
	font->m_Data = fontData;
	font->m_Flags = fontFlags;

	font->m_BackendData = fsBackendLoadFont(fs, fontData, dataSize);
	if (!font->m_BackendData) {
		return VG_INVALID_HANDLE;
	}

	// Store normalized line height. The real line height is got
	// by multiplying the lineh by font size.
	int32_t ascent, descent, lineGap;
	fsBackendGetFontVMetrics(font->m_BackendData, &ascent, &descent, &lineGap);
	int32_t fh = ascent - descent;
	font->m_Ascender = (float)ascent / (float)fh;
	font->m_Descender = (float)descent / (float)fh;
	font->m_LineHeight = (float)(fh + lineGap) / (float)fh;

	return handle;
}

FontHandle fsFindFont(const FontSystem* fs, const char* name)
{
	const bx::StringView nameSV(name);
	const uint32_t numFonts = fs->m_NumFonts;
	for (uint32_t i = 0; i < numFonts; ++i) {
		if (!bx::strCmp(fs->m_Fonts[i].m_Name, nameSV)) {
			return { (uint16_t)i };
		}
	}

	return VG_INVALID_HANDLE;
}

bool fsAddFallbackFont(FontSystem* fs, FontHandle baseFontHandle, FontHandle fallbackFontHandle)
{
	Font* baseFont = &fs->m_Fonts[baseFontHandle.idx];
	if (baseFont->m_NumFallbacks < FS_CONFIG_MAX_FALLBACK_FONTS) {
		baseFont->m_Fallback[baseFont->m_NumFallbacks] = fallbackFontHandle;
		++baseFont->m_NumFallbacks;

		return true;
	}

	return false;
}

static bool fsGetDirtyRect(FontSystem* fs, uint16_t* dirtyRect, bool reset)
{
	uint16_t* rect = &fs->m_DirtyRect[0];
	if (rect[0] < rect[2] && rect[1] < rect[3]) {
		bx::memCopy(dirtyRect, rect, sizeof(uint16_t) * 4);

		if (reset) {
			rect[0] = fs->m_Atlas->m_Width;
			rect[1] = fs->m_Atlas->m_Height;
			rect[2] = 0;
			rect[3] = 0;
		}

		return true;
	}

	return false;
}

const uint8_t* fsGetImageData(const FontSystem* fs, uint16_t* imageSize)
{
	if (imageSize) {
		imageSize[0] = fs->m_Atlas->m_Width;
		imageSize[1] = fs->m_Atlas->m_Height;
	}

	return fs->m_ImageData;
}

const uv_t* fsGetWhitePixelUV(const FontSystem* fs)
{
	return fs->m_FontImageWhitePixelUV;
}

ImageHandle fsGetFontAtlasImage(const FontSystem* fs)
{
	return fs->m_FontImages[fs->m_FontImageID];
}

void fsFlushFontAtlasImage(FontSystem* fs, vg::Context* ctx)
{
	uint16_t dirtyRect[4];
	if (!fsGetDirtyRect(fs, &dirtyRect[0], true)) {
		return;
	}

	// Update texture
	ImageHandle fontImage = fs->m_FontImages[fs->m_FontImageID];
	if (!vg::isValid(fontImage)) {
		return;
	}

	uint16_t imgSize[2];
	const uint8_t* a8Data = fsGetImageData(fs, &imgSize[0]);

	// TODO: Convert only the dirty part of the texture (it's the only part that will be uploaded to the backend)
	uint32_t* rgbaData = (uint32_t*)bx::alloc(fs->m_Allocator, sizeof(uint32_t) * imgSize[0] * imgSize[1]);
	vgutil::convertA8_to_RGBA8(rgbaData, a8Data, (uint32_t)imgSize[0], (uint32_t)imgSize[1], 0x00FFFFFF);

	vg::updateImage(ctx, fontImage, dirtyRect[0], dirtyRect[1], dirtyRect[2] - dirtyRect[0], dirtyRect[3] - dirtyRect[1], (const uint8_t*)rgbaData);

	bx::free(fs->m_Allocator, rgbaData);
}

uint32_t fsText(FontSystem* fs, vg::Context* ctx, const vg::TextConfig& cfg, const char* str, uint32_t len, uint32_t flags, TextMesh* mesh)
{
	VG_CHECK(vg::isValid(cfg.m_FontHandle), "Invalid font handle");

	bx::memSet(mesh, 0, sizeof(TextMesh));

	if (len == 0) {
		return 0;
	}

	const int16_t isize = (int16_t)(cfg.m_FontSize * 10.0f);
	if (isize < 2) {
		return 0; // Font size too small. Don't render anything.
	}

	TextBuffer* tb = &fs->m_TextBuffer;
	if (!fsTextBufferReset(tb, len, fs->m_Allocator)) {
		return 0; // Failed to allocate enough memory for text buffer.
	}

	// Convert UTF-8 string to codepoints.
	{
		uint32_t utf8State = 0;
		uint32_t* codepointPtr = tb->m_Codepoints;
		uint8_t* codepointSize = tb->m_CodepointSize;
		uint32_t codepointStartID = 0;
		for (uint32_t i = 0; i < len; ++i) {
			if (decodeUTF8(&utf8State, codepointPtr, (uint8_t)str[i])) {
				continue;
			}

			*codepointSize = (uint8_t)(i - codepointStartID) + 1;
			++codepointSize;
			++codepointPtr;
			codepointStartID = i + 1;
		}
		tb->m_Size = (uint32_t)(codepointPtr - tb->m_Codepoints);
	}

	return fsTextBuildMesh(fs, tb, ctx, cfg, flags, mesh);
}

static uint32_t fsTextBuildMesh(FontSystem* fs, TextBuffer* tb, vg::Context* ctx, const vg::TextConfig& cfg, uint32_t flags, TextMesh* mesh)
{
	const int16_t isize = (int16_t)(cfg.m_FontSize * 10.0f);

	// Check if there are actually any codepoints. This can happen if the input string contains an incomplete utf8 character (?).
	const uint32_t numCodepoints = tb->m_Size;
	if (numCodepoints == 0) {
		return 0;
	}

	// Find glyph indices for each codepoint
	{
		const vg::FontHandle fontHandle = cfg.m_FontHandle;
		Font* font = &fs->m_Fonts[fontHandle.idx];
		for (uint32_t i = 0; i < numCodepoints; ++i) {
			// Replace tabs with spaces. The same check is done below when deciding the width of the glyph.
			// Tabs are stretched spaces.
			const uint32_t codepoint = tb->m_Codepoints[i] == '\t' 
				? ' ' 
				: tb->m_Codepoints[i]
				;

			int32_t glyphIndex = fsBackendGetGlyphIndex(font->m_BackendData, codepoint);
			tb->m_GlyphIndices[i] = glyphIndex;
			tb->m_GlyphFonts[i] = fontHandle;

			if (glyphIndex == 0) {
				// Find glyph in fallback font.
				const uint32_t numFallbacks = font->m_NumFallbacks;
				for (uint32_t j = 0; j < numFallbacks; ++j) {
					const vg::FontHandle fallbackHandle = font->m_Fallback[j];
					glyphIndex = fsBackendGetGlyphIndex(fs->m_Fonts[fallbackHandle.idx].m_BackendData, codepoint);
					if (glyphIndex != 0) {
						tb->m_GlyphIndices[i] = glyphIndex;
						tb->m_GlyphFonts[i] = fallbackHandle;
						break;
					}
				}
			}
		}
	}

	// Calculate kerning
	{
		int32_t prevGlyphIndex = tb->m_GlyphIndices[0];
		vg::FontHandle prevFontHandle = tb->m_GlyphFonts[0];
		tb->m_KernAdv[0] = 0;
		for (uint32_t i = 1; i < numCodepoints; ++i) {
			const FontHandle glyphFont = tb->m_GlyphFonts[i];
			const int32_t glyphIndex = tb->m_GlyphIndices[i];

			tb->m_KernAdv[i] = (glyphFont.idx == prevFontHandle.idx)
				? fsBackendGetGlyphKernAdvance(fs->m_Fonts[prevFontHandle.idx].m_BackendData, prevGlyphIndex, glyphIndex)
				: 0 // TODO(JD): User specified pre and post kernings for fallback fonts?
				;

			prevGlyphIndex = glyphIndex;
			prevFontHandle = glyphFont;
		}
	}

	// Generate a quad for each codepoint
	float cursorX = 0.0f;
	float cursorY = 0.0f;
	float minx = 0.0f, maxx = 0.0f;
	float miny = 0.0f, maxy = 0.0f;
	{
		const Font* font = &fs->m_Fonts[cfg.m_FontHandle.idx];
		const bool originTopLeft = (fs->m_Config.m_Flags & FontSystemFlags::Origin_Msk) == FontSystemFlags::Origin_TopLeft;
		const float y_mult = originTopLeft ? 1.0f : -1.0f;
		const uint32_t bboxMinYID = originTopLeft ? 1 : 3;
		const uint32_t bboxMaxYID = originTopLeft ? 3 : 1;
		const int16_t iblur = (int16_t)bx::clamp<float>(cfg.m_Blur, 0.0f, 20.0f);
#if VG_CONFIG_UV_INT16
		const float x_to_u = (float)INT16_MAX / (float)fs->m_Atlas->m_Width;
		const float y_to_v = (float)INT16_MAX / (float)fs->m_Atlas->m_Height;
#else
		const float x_to_u = 1.0f / (float)fs->m_Atlas->m_Width;
		const float y_to_v = 1.0f / (float)fs->m_Atlas->m_Height;
#endif
		const float scale = fsBackendGetPixelHeightScale(font->m_BackendData, (float)isize / 10.0f);
		const float spacing = cfg.m_Spacing;

		const bool bitmapsOptional = (flags & TextFlags::BuildBitmaps) == 0;

		for (uint32_t i = 0; i < numCodepoints; ++i) {
			const FontHandle glyphFont = tb->m_GlyphFonts[i];
			const int32_t glyphIndex = tb->m_GlyphIndices[i];
			const uint32_t codepoint = tb->m_Codepoints[i];

			Glyph* glyph = fsBakeGlyph(fs, &fs->m_Fonts[glyphFont.idx], glyphIndex, codepoint, isize, iblur, bitmapsOptional);
			if (!glyph) {
				if (!fsAllocTextAtlas(fs, ctx)) {
					VG_WARN(false, "Failed to allocate enough text atlas space for string");
					return 0;
				}

				// Start from the beginning...
				i = 0;
				continue;
			}

			const int32_t kernAdv = tb->m_KernAdv[i];
			cursorX += FS_SNAP_COORD(((float)kernAdv * scale) + spacing);
			const float width_mult = codepoint == '\t' ? FS_CONFIG_TAB_SIZE : 1.0f;

			// Generate quad
			{
				TextQuad* q = &tb->m_Quads[i];

				// Each glyph has 2px border to allow good interpolation,
				// one pixel to prevent leaking, and one to allow good interpolation for rendering.
				// Inset the texture region by one pixel for correct interpolation.
				const float xoff = (float)(glyph->m_XOff + 1);
				const float yoff = (float)(glyph->m_YOff + 1);
				const float atlasMinX = (float)((int32_t)glyph->m_RectPos[0] + 1);
				const float atlasMinY = (float)((int32_t)glyph->m_RectPos[1] + 1);
				const float atlasMaxX = (float)((int32_t)glyph->m_RectPos[0] + (int32_t)glyph->m_RectSize[0] - 1);
				const float atlasMaxY = (float)((int32_t)glyph->m_RectPos[1] + (int32_t)glyph->m_RectSize[1] - 1);
				const float rx = FS_SNAP_COORD(cursorX + xoff);
				const float ry = FS_SNAP_COORD(cursorY + yoff * y_mult);

				// Positions
				q->m_Pos[0] = rx;
				q->m_Pos[1] = ry;
				q->m_Pos[2] = rx + (atlasMaxX - atlasMinX) * width_mult;
				q->m_Pos[3] = ry + (atlasMaxY - atlasMinY) * y_mult;

				if (glyphFont.idx != cfg.m_FontHandle.idx) {
					const Font* fallbackFont = &fs->m_Fonts[glyphFont.idx];
					const float deltaY = (fallbackFont->m_Descender - font->m_Descender) * cfg.m_FontSize * y_mult;
					q->m_Pos[1] += deltaY;
					q->m_Pos[3] += deltaY;
				}

				// UVs
				q->m_TexCoord[0] = (uv_t)(atlasMinX * x_to_u);
				q->m_TexCoord[1] = (uv_t)(atlasMinY * y_to_v);
				q->m_TexCoord[2] = (uv_t)(atlasMaxX * x_to_u);
				q->m_TexCoord[3] = (uv_t)(atlasMaxY * y_to_v);

				// Update bounding box
				miny = bx::min<float>(miny, q->m_Pos[bboxMinYID]);
				maxy = bx::max<float>(maxy, q->m_Pos[bboxMaxYID]);
			}

			cursorX += FS_SNAP_COORD(((float)glyph->m_XAdv / 10.0f) * width_mult + spacing);
		}

		// Calculate x bounds here. No need to do it inside the loop.
		minx = tb->m_Quads[0].m_Pos[0];
		maxx = tb->m_Quads[numCodepoints - 1].m_Pos[2];
	}

	float dx = 0.0f;
	const float width = cursorX;
	switch ((cfg.m_Alignment & VG_TEXT_ALIGN_HOR_Msk) >> VG_TEXT_ALIGN_HOR_Pos) {
	case vg::TextAlignHor::Left:
		break;
	case vg::TextAlignHor::Center:
		dx -= width * 0.5f;
		minx -= width * 0.5f;
		maxx -= width * 0.5f;
		break;
	case vg::TextAlignHor::Right:
		dx -= width;
		minx -= width;
		maxx -= width;
		break;
	}

	const float dy = fsGetVertAlign(fs, &fs->m_Fonts[cfg.m_FontHandle.idx], cfg.m_Alignment, isize);

	mesh->m_Alignment[0] = dx;
	mesh->m_Alignment[1] = dy;
	mesh->m_Bounds[0] = minx;
	mesh->m_Bounds[1] = miny;
	mesh->m_Bounds[2] = maxx + cfg.m_Spacing; // Make sure the bounding box includes the specified spacing at the end.
	mesh->m_Bounds[3] = maxy;
	mesh->m_Quads = tb->m_Quads;
	mesh->m_Codepoints = tb->m_Codepoints;
	mesh->m_CodepointSize = tb->m_CodepointSize;
	mesh->m_Size = numCodepoints;
	mesh->m_Width = width;

	return numCodepoints;
}

float fsGetLineHeight(FontSystem* fs, const vg::TextConfig& cfg)
{
	const Font* font = &fs->m_Fonts[cfg.m_FontHandle.idx];
	return font->m_LineHeight * (float)((int32_t)(cfg.m_FontSize * 10.0f)) / 10.0f;
}

static uint32_t decodeCodepoint(const char** str, const char* end)
{
	uint32_t utf8State = 0;
	uint32_t codepoint = 0;
	while (*str != end) {
		decodeUTF8(&utf8State, &codepoint, (uint8_t)(*str)[0]);
		++(*str);
		if (!utf8State) {
			return codepoint;
		}
	}

	return 0;
}

static inline bool isWhitespace(uint32_t codepoint)
{
	return false
		|| codepoint == ' ' 
		|| codepoint == '\t'
		;
}

// NOTE: Differs from the original NanoVG equivalent on the way min/max row boundaries are calculated.
// It does not take into account the X offset of glyphs. { minx, maxx } are always equal to { 0.0f, width }
uint32_t fsTextBreakLines(FontSystem* fs, const vg::TextConfig& cfg, const char* str, const char* end, float breakRowWidth, TextRow* rows, uint32_t maxRows, uint32_t textBreakFlags)
{
	if (maxRows < 2) {
		VG_CHECK(false, "At least 2 rows are needed.");
		return 0;
	}

	if (str == end) {
		return 0;
	}

	// Decode UTF-8 string while searching for mandatory breaks. Every time a mandatory break is found, generate quads for 
	// the line and try to fit as many characters/words possible to each row.
	TextBuffer* tb = &fs->m_TextBuffer;

	if (!fsTextBufferReset(tb, 0, fs->m_Allocator)) {
		return 0;
	}

	const bool keepTrailingSpaces = (textBreakFlags & TextBoxFlags::KeepTrailingSpaces) != 0;

	const char* cursor = str;
	const char* rowStart = str;
	uint32_t numRows = 0;
	uint32_t prevCodepoint = UINT32_MAX;
	while (cursor <= end && prevCodepoint != 0) {
		const char* nextCursor = cursor;
		const uint32_t codepoint = decodeCodepoint(&nextCursor, end);

		bool newline = false;
		switch (codepoint) {
		case 0x0000: // Null character
			newline = true;
			break;
		case 0x000A: // Line Feed (LF)
		case 0x000B: // Line Tabulation (VT)
		case 0x000C: // Form Feed (FF)
		case 0x0085: // Next Line (NEL)
		case 0x2028: // Line Separator
		case 0x2029: // Paragraph Separator
			newline = true;
			break;
		case 0x000D: { // Carriage Return (CR)
			const char* lookaheadCursor = nextCursor;
			if (decodeCodepoint(&lookaheadCursor, end) == 0x000A) {
				// CR LF => Skip both
				nextCursor = lookaheadCursor;
			}
			newline = true;
		} break;
		default:
			break;
		}

		if (newline) {
			// Calculate quads for the text buffer so far...
			TextMesh mesh;
			bx::memSet(&mesh, 0, sizeof(TextMesh));
			fsTextBuildMesh(fs, tb, nullptr, cfg, 0, &mesh);

			// Build rows.
			TextRow* row = &rows[numRows];
			row->start = rowStart;
			row->end = cursor; // Row ends before the line break.
			row->next = nextCursor;
			row->width = mesh.m_Width;
			row->minx = 0.0f;
			row->maxx = mesh.m_Width;
			++numRows;

			const uint32_t numCodepoints = tb->m_Size;
			if (numCodepoints != 0) {
				if (mesh.m_Width < breakRowWidth) {
					if (!keepTrailingSpaces) {
						uint32_t i = numCodepoints;
						while (i != 0 && isWhitespace(tb->m_Codepoints[i - 1])) {
							--i;
						}
						const float rowWidth = mesh.m_Quads[i - 1].m_Pos[2] - mesh.m_Quads[0].m_Pos[0];
						row->width = rowWidth;
						row->maxx = rowWidth;
					}
				} else {
					float rowStartX = tb->m_Quads[0].m_Pos[0];

					uint32_t nextCodepointPos = tb->m_CodepointSize[0];
					for (uint32_t i = 1; i < numCodepoints; ++i) {
						const float width = tb->m_Quads[i].m_Pos[2] - rowStartX;
						if (width > breakRowWidth) {
							// i-th glyph does not fit in current row. Find previous space.
							const uint32_t breakCodepointID = i - 1;
							const uint32_t breakNextCodepointPos = nextCodepointPos;
							{
								--i;
								while (i != 0 && !isWhitespace(tb->m_Codepoints[i])) {
									nextCodepointPos -= tb->m_CodepointSize[i];
									--i;
								}
							}

							if (i == 0) {
								// No spaces found from the break position to the start of the row.
								// Single word too big for the specified break width. Break at this point.
								i = breakCodepointID;
								nextCodepointPos = breakNextCodepointPos;
							} else {
								if (!keepTrailingSpaces) {
									// Client does need the trailing spaces of each row. Find the last non-whitespace character.
									// TODO: What will happen if the whole line is full of spaces?
									while (i < numCodepoints && isWhitespace(tb->m_Codepoints[i])) {
										nextCodepointPos += tb->m_CodepointSize[i];
										++i;
									}
								}
							}

							// Set row end and calculate bounds.
							{
								row->end = &rowStart[nextCodepointPos - tb->m_CodepointSize[i]];

								const float rowWidth = tb->m_Quads[i].m_Pos[2] - rowStartX;
								row->minx = 0.0f;
								row->maxx = rowWidth;
								row->width = rowWidth;
							}

							// Find next non-whitespace.
							{
								while (i < numCodepoints && isWhitespace(tb->m_Codepoints[i])) {
									nextCodepointPos += tb->m_CodepointSize[i];
									++i;
								}
							}

							row->next = &rowStart[nextCodepointPos - tb->m_CodepointSize[i]];

							if (numRows == maxRows) {
								return numRows;
							}

							rowStartX = tb->m_Quads[i].m_Pos[0];
							row = &rows[numRows];
							row->start = &rowStart[nextCodepointPos - tb->m_CodepointSize[i]];
							row->end = cursor;
							row->next = nextCursor;
							row->width = tb->m_Quads[numCodepoints - 1].m_Pos[2] - rowStartX;
							row->minx = 0.0f;
							row->maxx = row->width;
							++numRows;
						} else {
							nextCodepointPos += tb->m_CodepointSize[i];
						}
					}
				}
			}

			// Start a new row after the break.
			rowStart = nextCursor;

			if (numRows == maxRows) {
				break;
			}

			// Reset text buffer
			if (!fsTextBufferReset(tb, 0, fs->m_Allocator)) {
				return 0;
			}
		} else {
			if (!fsTextBufferPushCodepoint(tb, codepoint, (uint8_t)(nextCursor - cursor), fs->m_Allocator)) {
				return 0; // Failed to add codepoint to text buffer
			}
		}

		cursor = nextCursor;
		prevCodepoint = codepoint;
	}

	return numRows;
}

void fsLineBounds(FontSystem* fs, const vg::TextConfig& cfg, float y, float* miny, float* maxy)
{
	const Font* font = &fs->m_Fonts[cfg.m_FontHandle.idx];
	short isize = (short)(cfg.m_FontSize * 10.0f);

	y += fsGetVertAlign(fs, font, cfg.m_Alignment, isize);

	if ((fs->m_Config.m_Flags & FontSystemFlags::Origin_Msk) == FontSystemFlags::Origin_TopLeft) {
		*miny = y - font->m_Ascender * (float)isize / 10.0f;
		*maxy = *miny + font->m_LineHeight * isize / 10.0f;
	} else {
		*maxy = y + font->m_Descender * (float)isize / 10.0f;
		*miny = *maxy - font->m_LineHeight * isize / 10.0f;
	}
}

static bool fsAddWhiteRect(FontSystem* fs, uint16_t rectWidth, uint16_t rectHeight)
{
	uint16_t rectX, rectY;
	if (!fsAtlasAddRect(fs->m_Atlas, rectWidth, rectHeight, &rectX, &rectY)) {
		return false;
	}

	// Rasterize
	{
		const uint32_t atlasWidth = fs->m_Atlas->m_Width;
		uint8_t* dst = &fs->m_ImageData[(uint32_t)rectX + (uint32_t)rectY * atlasWidth];
		for (uint32_t y = 0; y < rectHeight; ++y) {
			bx::memSet(dst, 0xFF, rectWidth);
			dst += atlasWidth;
		}
	}

	fsInvalidateRect(fs, rectX, rectY, rectX + rectWidth, rectY + rectHeight);

	return true;
}

static void fsInvalidateRect(FontSystem* fs, uint16_t minX, uint16_t minY, uint16_t maxX, uint16_t maxY)
{
	fs->m_DirtyRect[0] = bx::min<uint16_t>(fs->m_DirtyRect[0], minX);
	fs->m_DirtyRect[1] = bx::min<uint16_t>(fs->m_DirtyRect[1], minY);
	fs->m_DirtyRect[2] = bx::max<uint16_t>(fs->m_DirtyRect[2], maxX);
	fs->m_DirtyRect[3] = bx::max<uint16_t>(fs->m_DirtyRect[3], maxY);
}

static FontHandle fsAllocFont(FontSystem* fs)
{
	if (fs->m_NumFonts == fs->m_FontCapacity) {
		const uint32_t oldCapacity = fs->m_FontCapacity;
		const uint32_t newCapacity = oldCapacity + 4;

		Font* newFonts = (Font*)bx::alloc(fs->m_Allocator, sizeof(Font) * newCapacity);
		if (!newFonts) {
			return VG_INVALID_HANDLE;
		}

		bx::memCopy(&newFonts[0], fs->m_Fonts, sizeof(Font) * oldCapacity);
		bx::memSet(&newFonts[oldCapacity], 0, sizeof(Font) * (newCapacity - oldCapacity));

		bx::free(fs->m_Allocator, fs->m_Fonts);
		fs->m_Fonts = newFonts;
		fs->m_FontCapacity = newCapacity;
	}

	const uint32_t id = fs->m_NumFonts;
	++fs->m_NumFonts;

	Font* font = &fs->m_Fonts[id];
	bx::memSet(font, 0, sizeof(Font));
	for (uint32_t i = 0; i < FS_CONFIG_LUT_SIZE; ++i) {
		font->m_LUT[i] = -1;
	}

	for (uint32_t i = 0; i < FS_CONFIG_MAX_FALLBACK_FONTS; ++i) {
		font->m_Fallback[i] = VG_INVALID_HANDLE;
	}

	return { (uint16_t)id };
}

static bool fsAllocTextAtlas(FontSystem* fs, Context* ctx)
{
	fsFlushFontAtlasImage(fs, ctx);

	if (fs->m_FontImageID + 1 >= FS_CONFIG_MAX_FONT_IMAGES) {
		VG_WARN(false, "No more text atlases for this frame");
		return false;
	}

	// if next fontImage already have a texture
	uint16_t iw, ih;
	if (vg::isValid(fs->m_FontImages[fs->m_FontImageID + 1])) {
		vg::getImageSize(ctx, fs->m_FontImages[fs->m_FontImageID + 1], &iw, &ih);
	} else {
		// calculate the new font image size and create it.
		const bool imgSizeValid = vg::getImageSize(ctx, fs->m_FontImages[fs->m_FontImageID], &iw, &ih);
		VG_CHECK(imgSizeValid, "Invalid font atlas dimensions");
		BX_UNUSED(imgSizeValid);

		if (iw > ih) {
			ih *= 2;
		} else {
			iw *= 2;
		}

		const uint32_t maxTextureSize = fs->m_Config.m_MaxTextureSize;
		if (iw > maxTextureSize || ih > maxTextureSize) {
			iw = ih = (uint16_t)maxTextureSize;
		}

		fs->m_FontImages[fs->m_FontImageID + 1] = vg::createImage(ctx, iw, ih, fs->m_Config.m_FontAtlasImageFlags, nullptr);
	}

	++fs->m_FontImageID;
	fsUpdateWhitePixelUV(fs, ctx);

	fsResetAtlas(fs, iw, ih);

	return true;
}

static bool fsResetAtlas(FontSystem* fs, uint16_t width, uint16_t height)
{
	// Reset atlas
	fsAtlasReset(fs->m_Atlas, width, height);

	// Clear texture data.
	fs->m_ImageData = (uint8_t*)bx::realloc(fs->m_Allocator, fs->m_ImageData, width * height);
	if (!fs->m_ImageData) {
		return false;
	}
	bx::memSet(fs->m_ImageData, 0, width * height);

	// Reset dirty rect
	fs->m_DirtyRect[0] = width;
	fs->m_DirtyRect[1] = height;
	fs->m_DirtyRect[2] = 0;
	fs->m_DirtyRect[3] = 0;

	// Reset cached glyphs
	for (uint32_t i = 0; i < fs->m_NumFonts; ++i) {
		Font* font = &fs->m_Fonts[i];
		font->m_NumGlyphs = 0;
		for (uint32_t j = 0; j < FS_CONFIG_LUT_SIZE; j++) {
			font->m_LUT[j] = -1;
		}
	}

	fs->m_Config.m_AtlasWidth = width;
	fs->m_Config.m_AtlasHeight = height;

	// Add white rect at 0,0 for debug drawing.
	fsAddWhiteRect(fs, fs->m_Config.m_WhiteRectWidth, fs->m_Config.m_WhiteRectHeight);

	fs->m_AtlasID++;

	return 1;
}

// Atlas based on Skyline Bin Packer by Jukka Jylänki
static Atlas* fsCreateAtlas(bx::AllocatorI* allocator, uint16_t w, uint16_t h)
{
	Atlas* atlas = (Atlas*)bx::alloc(allocator, sizeof(Atlas));
	if (!atlas) {
		return nullptr;
	}
	
	bx::memSet(atlas, 0, sizeof(Atlas));
	atlas->m_Allocator = allocator;
	atlas->m_Width = w;
	atlas->m_Height = h;

	const uint32_t rootNodeID = fsAtlasAllocNode(atlas);
	if (rootNodeID == UINT32_MAX) {
		fsDestroyAtlas(atlas);
		return nullptr;
	}

	fsAtlasSetNode(atlas, rootNodeID, 0, 0, w);

	return atlas;
}

static void fsDestroyAtlas(Atlas* atlas)
{
	bx::AllocatorI* allocator = atlas->m_Allocator;
	bx::free(allocator, atlas->m_Nodes);
	bx::free(allocator, atlas);
}

static uint32_t fsAtlasAllocNode(Atlas* atlas)
{
	if (atlas->m_NumNodes == atlas->m_NodeCapacity) {
		const uint32_t oldCapacity = atlas->m_NodeCapacity;
		const uint32_t newCapacity = oldCapacity == 0
			? 64
			: oldCapacity * 2
			;

		AtlasNode* newNodes = (AtlasNode*)bx::alloc(atlas->m_Allocator, sizeof(AtlasNode) * newCapacity);
		if (!newNodes) {
			return UINT32_MAX;
		}

		bx::memCopy(&newNodes[0], atlas->m_Nodes, sizeof(AtlasNode) * oldCapacity);
		bx::memSet(&newNodes[oldCapacity], 0, sizeof(AtlasNode) * (newCapacity - oldCapacity));
		
		bx::free(atlas->m_Allocator, atlas->m_Nodes);
		atlas->m_Nodes = newNodes;
		atlas->m_NodeCapacity = newCapacity;
	}

	const uint32_t id = atlas->m_NumNodes;
	++atlas->m_NumNodes;
	return id;
}

static inline void fsAtlasSetNode(Atlas* atlas, uint32_t nodeID, uint16_t x, uint16_t y, uint16_t w)
{
	VG_CHECK(nodeID < atlas->m_NumNodes, "Invalid atlas node ID");
	AtlasNode* node = &atlas->m_Nodes[nodeID];
	node->m_X = x;
	node->m_Y = y;
	node->m_Width = w;
}

static bool fsAtlasInsertNode(Atlas* atlas, uint32_t nodeID, uint16_t x, uint16_t y, uint16_t w)
{
	// Make sure there is room for one more node.
	if (fsAtlasAllocNode(atlas) == UINT32_MAX) {
		return false;
	}

	// Move everything up one slot
	bx::memMove(&atlas->m_Nodes[nodeID + 1], &atlas->m_Nodes[nodeID], sizeof(AtlasNode) * (atlas->m_NumNodes - nodeID - 1));
	
	// Set the new node
	fsAtlasSetNode(atlas, nodeID, x, y, w);

	return true;
}

static void fsAtlasRemoveNode(Atlas* atlas, uint32_t nodeID)
{
	bx::memMove(&atlas->m_Nodes[nodeID], &atlas->m_Nodes[nodeID + 1], sizeof(AtlasNode) * (atlas->m_NumNodes - nodeID - 1));
	atlas->m_NumNodes--;
}

static bool fsAtlasAddRect(Atlas* atlas, uint16_t rectWidth, uint16_t rectHeight, uint16_t* rectX, uint16_t* rectY)
{
	uint32_t besth = atlas->m_Height;
	uint32_t bestw = atlas->m_Width;
	uint32_t besti = UINT32_MAX;
	uint32_t bestx = UINT32_MAX;
	uint32_t besty = UINT32_MAX;

	// Bottom left fit heuristic.
	for (uint32_t i = 0; i < atlas->m_NumNodes; ++i) {
		const uint32_t y = fsAtlasRectFits(atlas, i, rectWidth, rectHeight);
		if (y != UINT32_MAX) {
			const uint32_t y2 = y + rectHeight;
			if (y2 < besth || (y2 == besth && atlas->m_Nodes[i].m_Width < bestw)) {
				besti = i;
				bestw = atlas->m_Nodes[i].m_Width;
				besth = y2;
				bestx = atlas->m_Nodes[i].m_X;
				besty = y;
			}
		}
	}

	if (besti == UINT32_MAX) {
		return false;
	}

	// Perform the actual packing.
	if (!fsAtlasAddSkylineLevel(atlas, besti, (uint16_t)bestx, (uint16_t)besty, rectWidth, rectHeight)) {
		return false;
	}
	
	*rectX = (uint16_t)bestx;
	*rectY = (uint16_t)besty;

	return true;
}

static uint32_t fsAtlasRectFits(Atlas* atlas, uint32_t nodeID, uint16_t rectWidth, uint16_t rectHeight)
{
	// Checks if there is enough space at the location of skyline span 'i',
	// and return the max height of all skyline spans under that at that location,
	// (think tetris block being dropped at that position). Or -1 if no space found.
	uint16_t x = atlas->m_Nodes[nodeID].m_X;
	uint16_t y = atlas->m_Nodes[nodeID].m_Y;
	if (x + rectWidth > atlas->m_Width) {
		return UINT32_MAX;
	}

	int32_t spaceLeft = (int32_t)rectWidth;
	while (spaceLeft > 0) {
		if (nodeID == atlas->m_NumNodes) {
			return UINT32_MAX;
		}

		y = bx::max<uint16_t>(y, atlas->m_Nodes[nodeID].m_Y);
		if (y + rectHeight > atlas->m_Height) {
			return UINT32_MAX;
		}

		spaceLeft -= atlas->m_Nodes[nodeID].m_Width;
		++nodeID;
	}

	return (uint32_t)y;
}

static bool fsAtlasAddSkylineLevel(Atlas* atlas, uint32_t nodeID, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	// Insert new node
	if (!fsAtlasInsertNode(atlas, nodeID, x, y + h, w)) {
		return false;
	}

	// Delete skyline segments that fall under the shadow of the new segment.
	for (uint32_t i = nodeID + 1; i < atlas->m_NumNodes; ++i) {
		AtlasNode* curNode = &atlas->m_Nodes[i];
		const AtlasNode* prevNode = &atlas->m_Nodes[i - 1];

		const uint16_t prevNodeEndX = prevNode->m_X + prevNode->m_Width;
		if (curNode->m_X < prevNodeEndX) {
			const uint16_t shrink = prevNodeEndX - curNode->m_X;
			if (curNode->m_Width <= shrink) {
				fsAtlasRemoveNode(atlas, i);
				--i;
			} else {
				curNode->m_X += shrink;
				curNode->m_Width -= shrink;
				break;
			} 
		} else {
			break;
		}
	}

	// Merge same height skyline segments that are next to each other.
	for (uint32_t i = 0; i < atlas->m_NumNodes - 1; ++i) {
		AtlasNode* curNode = &atlas->m_Nodes[i];
		const AtlasNode* nextNode = &atlas->m_Nodes[i + 1];

		if (curNode->m_Y == nextNode->m_Y) {
			curNode->m_Width += nextNode->m_Width;
			fsAtlasRemoveNode(atlas, i + 1);
			--i;
		}
	}

	return true;
}

static void fsAtlasReset(Atlas* atlas, uint16_t w, uint16_t h)
{
	atlas->m_Width = w;
	atlas->m_Height = h;
	atlas->m_NumNodes = 0;

	// Init root node.
	atlas->m_Nodes[0].m_X = 0;
	atlas->m_Nodes[0].m_Y = 0;
	atlas->m_Nodes[0].m_Width = (short)w;
	atlas->m_NumNodes++;
}

// Copyright (c) 2008-2010 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.

#define FONS_UTF8_ACCEPT 0

static uint32_t decodeUTF8(uint32_t* state, uint32_t* codep, uint8_t byte)
{
	static const uint8_t utf8d[] = {
		// The first part of the table maps bytes to character classes that
		// to reduce the size of the transition table and create bitmasks.
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
		7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
		8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
		10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,

		// The second part is a transition table that maps a combination
		// of a state of the automaton and a character class to a state.
		0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
		12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
		12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
		12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
		12,36,12,12,12,12,12,12,12,12,12,12,
	};

	const uint32_t type = utf8d[byte];

	*codep = (*state != FONS_UTF8_ACCEPT) 
		? (byte & 0x3fu) | (*codep << 6) 
		: (0xff >> type) & (byte)
		;

	*state = utf8d[256 + *state + type];
	return *state;
}

static void fsUpdateWhitePixelUV(FontSystem* fs, vg::Context* ctx)
{
	uint16_t w, h;
	getImageSize(ctx, fs->m_FontImages[fs->m_FontImageID], &w, &h);

#if VG_CONFIG_UV_INT16
	fs->m_FontImageWhitePixelUV[0] = INT16_MAX / (int16_t)w;
	fs->m_FontImageWhitePixelUV[1] = INT16_MAX / (int16_t)h;
#else
	fs->m_FontImageWhitePixelUV[0] = 0.5f / (float)w;
	fs->m_FontImageWhitePixelUV[1] = 0.5f / (float)h;
#endif
}

static void fsTextBufferInit(TextBuffer* tb)
{
	bx::memSet(tb, 0, sizeof(TextBuffer));
}

static void fsTextBufferShutdown(TextBuffer* tb, bx::AllocatorI* allocator)
{
	bx::alignedFree(allocator, tb->m_Buffer, 16);
	bx::memSet(tb, 0, sizeof(TextBuffer));
}

static bool fsTextBufferExpand(TextBuffer* tb, uint32_t newCapacity, bool keepOldData, bx::AllocatorI* allocator)
{
	VG_CHECK(newCapacity > tb->m_Capacity, "Don't call expand with a smaller capacity.");

	const uint32_t totalMemory = 0
		+ bx::strideAlign(sizeof(TextQuad) * newCapacity, 16)   // m_Quads
		+ bx::strideAlign(sizeof(uint32_t) * newCapacity, 16)   // m_Codepoints
		+ bx::strideAlign(sizeof(uint8_t) * newCapacity, 16)    // m_CodepointSize
		+ bx::strideAlign(sizeof(int32_t) * newCapacity, 16)    // m_GlyphIndices
		+ bx::strideAlign(sizeof(int32_t) * newCapacity, 16)    // m_KernAdv
		+ bx::strideAlign(sizeof(FontHandle) * newCapacity, 16) // m_GlyphFonts
		;

	uint8_t* buffer = (uint8_t*)bx::alignedAlloc(allocator, totalMemory, 16);
	if (!buffer) {
		return false;
	}

	uint8_t* ptr = buffer;
	TextQuad* newQuads = (TextQuad*)ptr;           ptr += bx::strideAlign(sizeof(TextQuad) * newCapacity, 16);
	uint32_t* newCodepoints = (uint32_t*)ptr;      ptr += bx::strideAlign(sizeof(uint32_t) * newCapacity, 16);
	uint8_t* newCodepointSizes = (uint8_t*)ptr;    ptr += bx::strideAlign(sizeof(uint8_t) * newCapacity, 16);
	int32_t* newGlyphIndices = (int32_t*)ptr;      ptr += bx::strideAlign(sizeof(int32_t) * newCapacity, 16);
	int32_t* newKernAdv = (int32_t*)ptr;           ptr += bx::strideAlign(sizeof(int32_t) * newCapacity, 16);
	FontHandle* newGlyphFonts = (FontHandle*)ptr;  ptr += bx::strideAlign(sizeof(FontHandle) * newCapacity, 16);

	if (keepOldData) {
		const uint32_t oldCapacity = tb->m_Capacity;
		bx::memCopy(newQuads, tb->m_Quads, sizeof(TextQuad) * oldCapacity);
		bx::memCopy(newCodepoints, tb->m_Codepoints, sizeof(uint32_t) * oldCapacity);
		bx::memCopy(newCodepointSizes, tb->m_CodepointSize, sizeof(uint8_t) * oldCapacity);
		bx::memCopy(newGlyphIndices, tb->m_GlyphIndices, sizeof(int32_t) * oldCapacity);
		bx::memCopy(newKernAdv, tb->m_KernAdv, sizeof(int32_t) * oldCapacity);
		bx::memCopy(newGlyphFonts, tb->m_GlyphFonts, sizeof(FontHandle) * oldCapacity);
	}

	if (tb->m_Buffer) {
		bx::alignedFree(allocator, tb->m_Buffer, 16);
	}
	tb->m_Buffer = buffer;
	tb->m_Quads = newQuads;
	tb->m_Codepoints = newCodepoints;
	tb->m_CodepointSize = newCodepointSizes;
	tb->m_GlyphIndices = newGlyphIndices;
	tb->m_KernAdv = newKernAdv;
	tb->m_GlyphFonts = newGlyphFonts;
	tb->m_Capacity = newCapacity;

	return true;
}

static bool fsTextBufferReset(TextBuffer* tb, uint32_t capacity, bx::AllocatorI* allocator)
{
	if (capacity > tb->m_Capacity) {
		if (!fsTextBufferExpand(tb, bx::strideAlign(capacity, 64), false, allocator)) {
			return false;
		}
	}

	tb->m_Size = 0;

	return true;
}

static bool fsTextBufferPushCodepoint(TextBuffer* tb, uint32_t codepoint, uint8_t codepointSize, bx::AllocatorI* allocator)
{
	if (tb->m_Size == tb->m_Capacity) {
		if (!fsTextBufferExpand(tb, bx::strideAlign(tb->m_Capacity + 64, 64), true, allocator)) {
			return false;
		}
	}

	const uint32_t id = tb->m_Size;
	tb->m_Codepoints[id] = codepoint;
	tb->m_CodepointSize[id] = codepointSize;
	++tb->m_Size;
	return true;
}

static float fsGetVertAlign(FontSystem* fs, const Font* font, uint32_t align, int16_t isize)
{
	if ((fs->m_Config.m_Flags & FontSystemFlags::Origin_Msk) == FontSystemFlags::Origin_TopLeft) {
		switch ((align & VG_TEXT_ALIGN_VER_Msk) >> VG_TEXT_ALIGN_VER_Pos) {
		case vg::TextAlignVer::Top:
			return font->m_Ascender * (float)isize / 10.0f;
		case vg::TextAlignVer::Middle:
			return (font->m_Ascender + font->m_Descender) / 2.0f * (float)isize / 10.0f;
		case vg::TextAlignVer::Baseline:
			return 0.0f;
		case vg::TextAlignVer::Bottom:
			return font->m_Descender * (float)isize / 10.0f;
		}
	} else {
		switch ((align & VG_TEXT_ALIGN_VER_Msk) >> VG_TEXT_ALIGN_VER_Pos) {
		case vg::TextAlignVer::Top:
			return -font->m_Ascender * (float)isize / 10.0f;
		case vg::TextAlignVer::Middle:
			return -(font->m_Ascender + font->m_Descender) / 2.0f * (float)isize / 10.0f;
		case vg::TextAlignVer::Baseline:
			return 0.0f;
		case vg::TextAlignVer::Bottom:
			return -font->m_Descender * (float)isize / 10.0f;
		}
	}

	return 0.0f;
}

static uint32_t fsHashGlyphCode(uint64_t glyphCode)
{
	// BKDR
	const char* c = (const char*)&glyphCode;
	unsigned int seed = 131; /* 31 131 1313 13131 131313 etc.. */

	unsigned int hash = (*c++);
	hash = (hash * seed) + (*c++);
	hash = (hash * seed) + (*c++);
	hash = (hash * seed) + (*c++);
	hash = (hash * seed) + (*c++);
	hash = (hash * seed) + (*c++);
	hash = (hash * seed) + (*c++);
	hash = (hash * seed) + (*c);

	return hash;
}

static Glyph* fsFontFindGlyph(Font* font, uint32_t codepoint, int16_t isize, int16_t iblur)
{
	const uint64_t glyphCode = FS_MAKE_GLYPH_CODE(codepoint, isize, iblur);
	const uint32_t hash = fsHashGlyphCode(glyphCode) & (FS_CONFIG_LUT_SIZE - 1);

	int32_t id = font->m_LUT[hash];
	while (id != -1) {
		if (font->m_Glyphs[id].m_GlyphCode == glyphCode) {
			return &font->m_Glyphs[id];
		}

		id = font->m_Glyphs[id].m_Next;
	}

	return nullptr;
}

// Based on Exponential blur, Jani Huhtanen, 2006

#define APREC 16
#define ZPREC 7

static void fsBlurCols(uint8_t* dst, int32_t w, int32_t h, int32_t dstStride, int32_t alpha)
{
	for (int32_t y = 0; y < h; ++y) {
		int32_t z = 0; // force zero border
		for (int32_t x = 1; x < w; ++x) {
			z += (alpha * (((int32_t)(dst[x]) << ZPREC) - z)) >> APREC;
			dst[x] = (uint8_t)(z >> ZPREC);
		}
		dst[w - 1] = 0; // force zero border
		z = 0;
		for (int32_t x = w - 2; x >= 0; --x) {
			z += (alpha * (((int32_t)(dst[x]) << ZPREC) - z)) >> APREC;
			dst[x] = (uint8_t)(z >> ZPREC);
		}
		dst[0] = 0; // force zero border
		dst += dstStride;
	}
}

static void fsBlurRows(uint8_t* dst, int32_t w, int32_t h, int32_t dstStride, int32_t alpha)
{
	for (int32_t x = 0; x < w; ++x) {
		int32_t z = 0; // force zero border
		for (int32_t y = dstStride; y < h * dstStride; y += dstStride) {
			z += (alpha * (((int32_t)(dst[y]) << ZPREC) - z)) >> APREC;
			dst[y] = (uint8_t)(z >> ZPREC);
		}
		dst[(h - 1) * dstStride] = 0; // force zero border
		z = 0;
		for (int32_t y = (h - 2) * dstStride; y >= 0; y -= dstStride) {
			z += (alpha * (((int32_t)(dst[y]) << ZPREC) - z)) >> APREC;
			dst[y] = (uint8_t)(z >> ZPREC);
		}
		dst[0] = 0; // force zero border
		dst++;
	}
}

static void fsBlur(uint8_t* dst, int32_t w, int32_t h, int32_t dstStride, int32_t blur)
{
	if (blur < 1) {
		return;
	}

	// Calculate the alpha such that 90% of the kernel is within the radius. (Kernel extends to infinity)
	const float sigma = (float)blur * 0.57735f; // 1 / sqrt(3)
	const int32_t alpha = (int32_t)((1 << APREC) * (1.0f - bx::exp(-2.3f / (sigma + 1.0f))));
	fsBlurRows(dst, w, h, dstStride, alpha);
	fsBlurCols(dst, w, h, dstStride, alpha);
	fsBlurRows(dst, w, h, dstStride, alpha);
	fsBlurCols(dst, w, h, dstStride, alpha);
}

static Glyph* fsBakeGlyph(FontSystem* fs, Font* font, int32_t glyphIndex, uint32_t codepoint, int16_t isize, int16_t iblur, bool glyphBitmapOptional)
{
	const float size = (float)isize / 10.0f;
	const int32_t pad = iblur + 2;

	Glyph* glyph = fsFontFindGlyph(font, codepoint, isize, iblur);
	if (glyph && (glyphBitmapOptional || (glyph->m_RectPos[0] != UINT16_MAX && glyph->m_RectPos[1] != UINT16_MAX))) {
		return glyph;
	}

	const float scale = fsBackendGetPixelHeightScale(font->m_BackendData, size);

	int advance, lsb, x0, y0, x1, y1;
	fsBackendBuildGlyphBitmap(font->m_BackendData, glyphIndex, size, scale, &advance, &lsb, &x0, &y0, &x1, &y1);

	const uint16_t gw = (uint16_t)(x1 - x0 + pad * 2);
	const uint16_t gh = (uint16_t)(y1 - y0 + pad * 2);

	// Find free spot for the rect in the atlas
	uint16_t gx, gy;
	if (!glyphBitmapOptional) {
		bool added = fsAtlasAddRect(fs->m_Atlas, gw, gh, &gx, &gy);
		if (!added) {
			return nullptr;
		}
	} else {
		gx = UINT16_MAX;
		gy = UINT16_MAX;
	}

	// Init glyph.
	if (glyph == nullptr) {
		glyph = fsAllocGlyph(fs, font);
		glyph->m_GlyphCode = FS_MAKE_GLYPH_CODE(codepoint, isize, iblur);

		// Insert char to hash lookup.
		const uint32_t h = fsHashGlyphCode(glyph->m_GlyphCode) & (FS_CONFIG_LUT_SIZE - 1);
		glyph->m_Next = font->m_LUT[h];
		font->m_LUT[h] = font->m_NumGlyphs - 1;
	}

	glyph->m_RectPos[0] = gx;
	glyph->m_RectPos[1] = gy;
	glyph->m_RectSize[0] = gw;
	glyph->m_RectSize[1] = gh;
	glyph->m_XAdv = (int16_t)(scale * advance * 10.0f);
	glyph->m_XOff = (int16_t)(x0 - pad);
	glyph->m_YOff = (int16_t)(y0 - pad);

	if (glyphBitmapOptional) {
		return glyph;
	}

	// Rasterize
	{
		const uint32_t atlasWidth = fs->m_Atlas->m_Width;

		uint8_t* dst = &fs->m_ImageData[(gx + pad) + (gy + pad) * atlasWidth];
		fsBackendRenderGlyphBitmap(font->m_BackendData, dst, gw - pad * 2, gh - pad * 2, atlasWidth, scale, scale, glyphIndex);

		// Make sure there is one pixel empty border.
		dst = &fs->m_ImageData[gx + gy * atlasWidth];
		for (uint32_t y = 0; y < gh; y++) {
			dst[y * atlasWidth] = 0;
			dst[gw - 1 + y * atlasWidth] = 0;
		}
		for (uint32_t x = 0; x < gw; x++) {
			dst[x] = 0;
			dst[x + (gh - 1) * atlasWidth] = 0;
		}
	}

	// Blur
	if (iblur > 0) {
		uint8_t* bdst = &fs->m_ImageData[gx + gy * fs->m_Atlas->m_Width];
		fsBlur(bdst, gw, gh, fs->m_Atlas->m_Width, iblur);
	}

	fsInvalidateRect(fs, glyph->m_RectPos[0], glyph->m_RectPos[1], glyph->m_RectPos[0] + glyph->m_RectSize[0], glyph->m_RectPos[1] + glyph->m_RectSize[1]);

	return glyph;
}

static Glyph* fsAllocGlyph(FontSystem* fs, Font* font)
{
	if (font->m_NumGlyphs + 1 > font->m_GlyphCapacity) {
		font->m_GlyphCapacity = font->m_GlyphCapacity == 0 
			? 8 
			: font->m_GlyphCapacity * 2
			;

		font->m_Glyphs = (Glyph*)bx::realloc(fs->m_Allocator, font->m_Glyphs, sizeof(Glyph) * font->m_GlyphCapacity);
		if (!font->m_Glyphs) {
			return nullptr;
		}
	}

	font->m_NumGlyphs++;
	return &font->m_Glyphs[font->m_NumGlyphs - 1];
}

//////////////////////////////////////////////////////////////////////////
// stbtt Backend
//
#include "libs/stb_truetype.h"

#define FS_STBTT_FIRST_GLYPH        0x20
#define FS_STBTT_LAST_GLYPH         0x7E
#define FS_STBTT_NUM_GLYPH_INDICES  (FS_STBTT_LAST_GLYPH - FS_STBTT_FIRST_GLYPH + 1)

struct FontStb
{
	stbtt_fontinfo m_Font;
	int m_GlyphIndex[FS_STBTT_NUM_GLYPH_INDICES];
	int* m_Kern;
	int m_MinGlyphIndex;
	int m_MaxGlyphIndex;
};

static bool fsBackendInit(FontSystem* fs)
{
	BX_UNUSED(fs);
	return true;
}

static void* fsBackendLoadFont(FontSystem* fs, uint8_t* data, uint32_t dataSize)
{
	BX_UNUSED(dataSize);

	bx::AllocatorI* allocator = fs->m_Allocator;

	FontStb* font = (FontStb*)bx::alloc(allocator, sizeof(FontStb));
	bx::memSet(font, 0, sizeof(FontStb));

	font->m_Font.userdata = nullptr;
	int32_t stbError = stbtt_InitFont(&font->m_Font, data, 0);
	if (!stbError) {
		bx::free(allocator, font);
		return nullptr;
	}

	int32_t minGlyphIndex = INT32_MAX;
	int32_t maxGlyphIndex = INT32_MIN;
	for (uint32_t cp = FS_STBTT_FIRST_GLYPH; cp <= FS_STBTT_LAST_GLYPH; ++cp) {
		const int32_t gi = stbtt_FindGlyphIndex(&font->m_Font, cp);
		font->m_GlyphIndex[cp - FS_STBTT_FIRST_GLYPH] = gi;

		if (gi < minGlyphIndex) {
			minGlyphIndex = gi;
		}
		if (gi > maxGlyphIndex) {
			maxGlyphIndex = gi;
		}
	}

	font->m_MinGlyphIndex = minGlyphIndex;
	font->m_MaxGlyphIndex = maxGlyphIndex;
	
	const uint32_t numGlyphs = maxGlyphIndex - minGlyphIndex + 1;
	const uint32_t totalPairs = numGlyphs * numGlyphs;
	font->m_Kern = (int32_t*)bx::alloc(allocator, sizeof(int32_t) * totalPairs);
	if (font->m_Kern) {
		for (int32_t second = minGlyphIndex; second <= maxGlyphIndex; ++second) {
			const int32_t mult = (second - minGlyphIndex) * numGlyphs;
			for (int32_t first = minGlyphIndex; first <= maxGlyphIndex; ++first) {
				font->m_Kern[first - minGlyphIndex + mult] = stbtt_GetGlyphKernAdvance(&font->m_Font, first, second);
			}
		}
	}

	return font;
}

static void fsBackendFreeFont(FontSystem* fs, void* fontPtr)
{
	FontStb* font = (FontStb*)fontPtr;
	if (!font) {
		return;
	}

	bx::AllocatorI* allocator = fs->m_Allocator;

	bx::free(allocator, font->m_Kern);
	bx::free(allocator, font);
}

static void fsBackendGetFontVMetrics(void* fontPtr, int32_t* ascent, int32_t* descent, int32_t* lineGap)
{
	FontStb* font = (FontStb*)fontPtr;
	stbtt_GetFontVMetrics(&font->m_Font, ascent, descent, lineGap);
}

static float fsBackendGetPixelHeightScale(void* fontPtr, float size)
{
	FontStb* font = (FontStb*)fontPtr;
#if FS_CONFIG_FONT_SIZE_EM
	return stbtt_ScaleForMappingEmToPixels(&font->m_Font, size);
#else
	return stbtt_ScaleForPixelHeight(&font->m_Font, size);
#endif
}

static int32_t fsBackendGetGlyphIndex(void* fontPtr, uint32_t codepoint)
{
	FontStb* font = (FontStb*)fontPtr;
	if (codepoint >= FS_STBTT_FIRST_GLYPH && codepoint <= FS_STBTT_LAST_GLYPH) {
		return font->m_GlyphIndex[codepoint - FS_STBTT_FIRST_GLYPH];
	}

	return stbtt_FindGlyphIndex(&font->m_Font, codepoint);
}

static bool fsBackendBuildGlyphBitmap(void* fontPtr, int32_t glyph, float size, float scale, int32_t* advance, int32_t* lsb, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1)
{
	BX_UNUSED(size);
	FontStb* font = (FontStb*)fontPtr;
	stbtt_GetGlyphHMetrics(&font->m_Font, glyph, advance, lsb);
	stbtt_GetGlyphBitmapBox(&font->m_Font, glyph, scale, scale, x0, y0, x1, y1);
	return true;
}

static void fsBackendRenderGlyphBitmap(void* fontPtr, uint8_t* output, int32_t outWidth, int32_t outHeight, int32_t outStride, float scaleX, float scaleY, int glyph)
{
	FontStb* font = (FontStb*)fontPtr;
	stbtt_MakeGlyphBitmap(&font->m_Font, output, outWidth, outHeight, outStride, scaleX, scaleY, glyph);
}

static int32_t fsBackendGetGlyphKernAdvance(void* fontPtr, int32_t glyph1, int32_t glyph2)
{
	FontStb* font = (FontStb*)fontPtr;
	const int minID = font->m_MinGlyphIndex;
	const int maxID = font->m_MaxGlyphIndex;
	if (glyph1 >= minID && glyph1 <= maxID && glyph2 >= minID && glyph2 <= maxID) {
		const int g1 = glyph1 - minID;
		const int g2 = glyph2 - minID;
		const int combo = g1 + g2 * (maxID - minID + 1);
		return font->m_Kern[combo];
	}

	return stbtt_GetGlyphKernAdvance(&font->m_Font, glyph1, glyph2);
}
}
