#ifndef VG_FONT_SYSTEM_H
#define VG_FONT_SYSTEM_H

#include <stdint.h>
#include <vg/vg.h>

namespace bx
{
struct AllocatorI;
}

namespace vg
{
struct FontSystemFlags
{
	enum Enum : uint32_t
	{
		None = 0,
		Origin_Msk = 0x01,
		Origin_TopLeft = 0,
		Origin_BottomLeft = 1
	};
};

struct FontSystemConfig
{
	uint16_t m_AtlasWidth;
	uint16_t m_AtlasHeight;
	uint16_t m_WhiteRectWidth;
	uint16_t m_WhiteRectHeight;
	uint32_t m_MaxTextureSize;
	uint32_t m_Flags;
	uint32_t m_FontAtlasImageFlags;
};

struct TextQuad
{
	float m_Pos[4];     // { x0, y0, x1, y1 }
	uv_t m_TexCoord[4]; // { s0, t0, s1, t1 }
#if VG_CONFIG_ENABLE_SIMD && VG_CONFIG_UV_INT16
	uint8_t _Padding[8];
#endif
};

#if VG_CONFIG_ENABLE_SIMD
BX_STATIC_ASSERT((sizeof(TextQuad) & 15) == 0, "TextQuad size must be a multiple of 16");
#endif

struct TextMesh
{
	const TextQuad* m_Quads;
	const uint32_t* m_Codepoints;
	const uint8_t* m_CodepointSize;
	uint32_t m_Size;
	float m_Width;
	float m_Alignment[2];
	float m_Bounds[4];
};

struct TextFlags
{
	enum Enum : uint32_t
	{
		BuildBitmaps = 1u << 0
	};
};

struct FontSystem;

FontSystem* fsCreate(vg::Context* ctx, bx::AllocatorI* allocator, const FontSystemConfig* cfg);
void fsDestroy(FontSystem* fs, vg::Context* ctx);
void fsFrame(FontSystem* fs, vg::Context* ctx);
FontHandle fsAddFont(FontSystem* fs, const char* name, uint8_t* data, uint32_t dataSize, uint32_t fontFlags);
FontHandle fsFindFont(const FontSystem* fs, const char* name);
bool fsAddFallbackFont(FontSystem* fs, FontHandle baseFont, FontHandle fallbackFont);
const uint8_t* fsGetImageData(const FontSystem* fs, uint16_t* imageSize);
void fsFlushFontAtlasImage(FontSystem* fs, vg::Context* ctx);
ImageHandle fsGetFontAtlasImage(const FontSystem* fs);
const uv_t* fsGetWhitePixelUV(const FontSystem* fs);

uint32_t fsText(FontSystem* fs, vg::Context* ctx, const vg::TextConfig& cfg, const char* str, uint32_t len, uint32_t flags, TextMesh* mesh);
void fsLineBounds(FontSystem* fs, const vg::TextConfig& cfg, float y, float* minY, float* maxY);
uint32_t fsTextBreakLines(FontSystem* fs, const vg::TextConfig& cfg, const char* str, const char* end, float breakRowWidth, TextRow* rows, uint32_t maxRows, uint32_t textBreakFlags);
float fsGetLineHeight(FontSystem* fs, const vg::TextConfig& cfg);
}

#endif
