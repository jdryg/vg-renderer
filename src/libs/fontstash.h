//
// Copyright (c) 2009-2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#ifndef FONS_H
#define FONS_H

#include <assert.h>

#define FONS_INVALID -1

// JD: Rearrange member of FONSquad (x, y coords are next to each other to simplify
// SIMD transform).
// Revert to original behavior with 0
#ifndef FONS_QUAD_SIMD
#	define FONS_QUAD_SIMD 1
#endif

// JD: If 0 packs codepoint, size and blur members of the FONSglyph struct into a single uint64_t.
// In this case a different hash function is also used to find the LUT bucket (hash takes into account glyph size and blur).
// Revert to original behavior with 1.
#ifndef FONS_SEPARATE_CODEPOINT
#	define FONS_SEPARATE_CODEPOINT 0
#endif

// JD: If 0 hides rendering related functions and members (smaller FONScontext).
// Revert to original behavior with 1
#ifndef FONS_RENDERING
#	define FONS_RENDERING 0
#endif

#ifndef FONS_CUSTOM_WHITE_RECT
#	define FONS_CUSTOM_WHITE_RECT 1
#endif

// JD: Revert to original behavior with 1
#ifndef FONS_SNAP_TO_GRID
#	define FONS_SNAP_TO_GRID 1
#endif

// JD: If 1 builds an array for mapping ASCII chars between 0x20 and 0x7E to
// glyph indices for the current font to avoid calling stbtt_FindGlyphIndex()
// for ASCII chars.
#ifndef FONS_ASCII_TO_GLYPH_INDEX_ARRAY
#	define FONS_ASCII_TO_GLYPH_INDEX_ARRAY 1
#	define FONS_FIRST_ASCII_CODEPOINT 0x20
#	define FONS_LAST_ASCII_CODEPOINT 0x7E
#	define FONS_NUM_ASCII_TO_GLYPH_INDICES (FONS_LAST_ASCII_CODEPOINT - FONS_FIRST_ASCII_CODEPOINT + 1)
#endif

// JD:
#ifndef FONS_GLYPH_KERN_ARRAY_ASCII
#if FONS_ASCII_TO_GLYPH_INDEX_ARRAY
#	define FONS_GLYPH_KERN_ARRAY_ASCII 1
#else
#	define FONS_GLYPH_KERN_ARRAY_ASCII 0
#endif
#endif

// MC:
#ifndef FONS_GLYPH_KERN_NONZERO_CODEMAP
#	define FONS_GLYPH_KERN_NONZERO_CODEMAP 1
#endif

#ifndef FONS_STDIO
#	define FONS_STDIO 0
#endif

#if !FONS_SEPARATE_CODEPOINT
#include <stdint.h> // uint64_t
#endif

enum FONSflags {
	FONS_ZERO_TOPLEFT = 1,
	FONS_ZERO_BOTTOMLEFT = 2,
};

enum FONSalign {
	// Horizontal align
	FONS_ALIGN_LEFT 	= 1<<0,	// Default
	FONS_ALIGN_CENTER 	= 1<<1,
	FONS_ALIGN_RIGHT 	= 1<<2,
	// Vertical align
	FONS_ALIGN_TOP 		= 1<<3,
	FONS_ALIGN_MIDDLE	= 1<<4,
	FONS_ALIGN_BOTTOM	= 1<<5,
	FONS_ALIGN_BASELINE	= 1<<6, // Default
};

enum FONSglyphBitmap
{
	FONS_GLYPH_BITMAP_OPTIONAL = 1,
	FONS_GLYPH_BITMAP_REQUIRED = 2,
};

enum FONSerrorCode {
	// Font atlas is full.
	FONS_ATLAS_FULL = 1,
	// Scratch memory used to render glyphs is full, requested size reported in 'val', you may need to bump up FONS_SCRATCH_BUF_SIZE.
	FONS_SCRATCH_FULL = 2,
	// Calls to fonsPushState has created too large stack, if you need deep state stack bump up FONS_MAX_STATES.
	FONS_STATES_OVERFLOW = 3,
	// Trying to pop too many states fonsPopState().
	FONS_STATES_UNDERFLOW = 4,
};

struct FONSparams {
	int width, height;
	unsigned char flags;
	void* userPtr;
#if FONS_CUSTOM_WHITE_RECT
	int whiteRectWidth, whiteRectHeight;
#endif
	int (*renderCreate)(void* uptr, int width, int height);
	int (*renderResize)(void* uptr, int width, int height);
	void (*renderUpdate)(void* uptr, int* rect, const unsigned char* data);
	void (*renderDraw)(void* uptr, const float* verts, const float* tcoords, const unsigned int* colors, int nverts);
	void (*renderDelete)(void* uptr);
};
typedef struct FONSparams FONSparams;

struct FONSquad
{
#if !FONS_QUAD_SIMD
	float x0,y0,s0,t0;
	float x1,y1,s1,t1;
#else
	float x0, y0, x1, y1;
	float s0, t0, s1, t1;
#endif
};
typedef struct FONSquad FONSquad;

struct FONStextIter {
	float x, y, nextx, nexty, scale, spacing;
	unsigned int codepoint;
	short isize, iblur;
	struct FONSfont* font;
	int prevGlyphIndex;
	const char* str;
	const char* next;
	const char* end;
	unsigned int utf8state;
	int bitmapOption;
};
typedef struct FONStextIter FONStextIter;

struct FONSstring
{
	FONSquad* m_Quads;
	unsigned int* m_Codepoints;
	int* m_GlyphIndices;
	int* m_KernAdv;
	unsigned int m_Length;
	unsigned int m_Capacity;
	int m_LastBakeAtlasID;
	float m_Bounds[4];
	float m_Width;
};
typedef struct FONSstring FONSstring;

typedef struct FONScontext FONScontext;

// Constructor and destructor.
FONScontext* fonsCreateInternal(FONSparams* params);
void fonsDeleteInternal(FONScontext* s);

void fonsSetErrorCallback(FONScontext* s, void (*callback)(void* uptr, int error, int val), void* uptr);
// Returns current atlas size.
void fonsGetAtlasSize(FONScontext* s, int* width, int* height);
// Expands the atlas size.
int fonsExpandAtlas(FONScontext* s, int width, int height);
// Resets the whole stash.
int fonsResetAtlas(FONScontext* stash, int width, int height);

// Add fonts
#if FONS_STDIO
int fonsAddFont(FONScontext* s, const char* name, const char* path);
#endif
int fonsAddFontMem(FONScontext* s, const char* name, unsigned char* data, int ndata, int freeData);
int fonsGetFontByName(FONScontext* s, const char* name);
int fonsAddFallbackFont(FONScontext* stash, int base, int fallback);

// State handling
void fonsPushState(FONScontext* s);
void fonsPopState(FONScontext* s);
void fonsClearState(FONScontext* s);

// State setting
void fonsSetSize(FONScontext* s, float size);
void fonsSetColor(FONScontext* s, unsigned int color);
void fonsSetSpacing(FONScontext* s, float spacing);
void fonsSetBlur(FONScontext* s, float blur);
void fonsSetAlign(FONScontext* s, int align);
void fonsSetFont(FONScontext* s, int font);

// Draw text
#if FONS_RENDERING
float fonsDrawText(FONScontext* s, float x, float y, const char* string, const char* end);
#endif

// Measure text
float fonsTextBounds(FONScontext* s, float x, float y, const char* string, const char* end, float* bounds);
void fonsLineBounds(FONScontext* s, float y, float* miny, float* maxy);
void fonsVertMetrics(FONScontext* s, float* ascender, float* descender, float* lineh);

// Text iterator
int fonsTextIterInit(FONScontext* stash, FONStextIter* iter, float x, float y, const char* str, const char* end, int bitmapOption);
int fonsTextIterNext(FONScontext* stash, FONStextIter* iter, struct FONSquad* quad);

// Pull texture changes
const unsigned char* fonsGetTextureData(FONScontext* stash, int* width, int* height);
int fonsValidateTexture(FONScontext* s, int* dirty);

// Strings
void fonsInitString(FONSstring* str);
void fonsDestroyString(FONSstring* str);
void fonsResetString(FONScontext* stash, FONSstring* str, const char* text, const char* end);
int fonsBakeString(FONScontext* stash, FONSstring* str);
float fonsAlignString(FONScontext* stash, FONSstring* str, unsigned int align, float* x, float* y);

// Draws the stash texture for debugging
#if FONS_RENDERING
void fonsDrawDebug(FONScontext* s, float x, float y);
#endif

#endif // FONTSTASH_H

#ifdef FONTSTASH_IMPLEMENTATION

#define FONS_NOTUSED(v) BX_UNUSED(v)

#if !FONS_SEPARATE_CODEPOINT
#define MAKE_GLYPH_CODE(cp, size, blur) (((uint64_t)(cp)) | ((uint64_t)(size) << 32) | ((uint64_t)(blur) << 48))
#endif

#ifdef FONS_USE_FREETYPE

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include <math.h>

#	include <stdlib.h>
#	include <string.h>
#ifndef FONSmalloc
#define FONSmalloc malloc
#endif
#ifndef FONSrealloc
#define FONSrealloc realloc
#endif
#ifndef FONSfree
#define FONSfree free
#endif

struct FONSttFontImpl {
	FT_Face font;
};
typedef struct FONSttFontImpl FONSttFontImpl;

static FT_Library ftLibrary;

int fons__tt_init(FONScontext *context)
{
	FT_Error ftError;
        FONS_NOTUSED(context);
	ftError = FT_Init_FreeType(&ftLibrary);
	return ftError == 0;
}

int fons__tt_loadFont(FONScontext *context, FONSttFontImpl *font, unsigned char *data, int dataSize)
{
	FT_Error ftError;
	FONS_NOTUSED(context);

	//font->font.userdata = stash;
	ftError = FT_New_Memory_Face(ftLibrary, (const FT_Byte*)data, dataSize, 0, &font->font);
	return ftError == 0;
}

void fons__tt_getFontVMetrics(FONSttFontImpl *font, int *ascent, int *descent, int *lineGap)
{
	*ascent = font->font->ascender;
	*descent = font->font->descender;
	*lineGap = font->font->height - (*ascent - *descent);
}

float fons__tt_getPixelHeightScale(FONSttFontImpl *font, float size)
{
	return size / (font->font->ascender - font->font->descender);
}

int fons__tt_getGlyphIndex(FONSttFontImpl *font, int codepoint)
{
	return FT_Get_Char_Index(font->font, codepoint);
}

int fons__tt_buildGlyphBitmap(FONSttFontImpl *font, FONSttFontImpl *baseFont, int glyph, float size, float scale, int *advance, int *lsb, int *x0, int *y0, int *x1, int *y1)
{
	FT_Error ftError;
	FT_GlyphSlot ftGlyph;
	FT_Fixed advFixed;
	FONS_NOTUSED(scale);

	const FT_UInt pxSize = (FT_UInt)(size * (float)font->font->units_per_EM / (float)(font->font->ascender - font->font->descender));
	const FT_UInt basePxSize = (FT_UInt)(size * (float)baseFont->font->units_per_EM / (float)(baseFont->font->ascender - baseFont->font->descender));

	ftError = FT_Set_Pixel_Sizes(font->font, 0, pxSize);
	if (ftError) return 0;
	ftError = FT_Load_Glyph(font->font, glyph, FT_LOAD_RENDER);
	if (ftError) return 0;
	ftError = FT_Get_Advance(font->font, glyph, FT_LOAD_NO_SCALE, &advFixed);
	if (ftError) return 0;
	ftGlyph = font->font->glyph;
	*advance = (int)advFixed;
	*lsb = (int)ftGlyph->metrics.horiBearingX;
	*x0 = ftGlyph->bitmap_left;
	*x1 = *x0 + ftGlyph->bitmap.width;
	*y0 = -ftGlyph->bitmap_top + ((int)pxSize - (int)basePxSize) / 2;
	*y1 = *y0 + ftGlyph->bitmap.rows;
	return 1;
}

void fons__tt_renderGlyphBitmap(FONSttFontImpl *font, unsigned char *output, int outWidth, int outHeight, int outStride,
								float scaleX, float scaleY, int glyph)
{
	FT_GlyphSlot ftGlyph = font->font->glyph;
	int ftGlyphOffset = 0;
	unsigned int x, y;
	FONS_NOTUSED(outWidth);
	FONS_NOTUSED(outHeight);
	FONS_NOTUSED(scaleX);
	FONS_NOTUSED(scaleY);
	FONS_NOTUSED(glyph);	// glyph has already been loaded by fons__tt_buildGlyphBitmap

	for ( y = 0; y < ftGlyph->bitmap.rows; y++ ) {
		for ( x = 0; x < ftGlyph->bitmap.width; x++ ) {
			output[(y * outStride) + x] = ftGlyph->bitmap.buffer[ftGlyphOffset++];
		}
	}
}

int fons__tt_getGlyphKernAdvance(FONSttFontImpl *font, int glyph1, int glyph2)
{
	FT_Vector ftKerning;
	FT_Get_Kerning(font->font, glyph1, glyph2, FT_KERNING_DEFAULT, &ftKerning);
	return (int)((ftKerning.x + 32) >> 6);  // Round up and convert to integer
}

#else

#if 0
#define STB_TRUETYPE_IMPLEMENTATION
#	define STBTT_malloc(x,u) fons__tmpalloc(x,u)
#	define STBTT_free(x,u)   fons__tmpfree(x,u)
static void* fons__tmpalloc(size_t size, void* up);
static void fons__tmpfree(void* ptr, void* up);
#else
#	include <stdlib.h>
#	include <string.h>
#ifndef FONSmalloc
#define FONSmalloc malloc
#endif
#ifndef FONSrealloc
#define FONSrealloc realloc
#endif
#ifndef FONSfree
#define FONSfree free
#endif
#endif // 0

#define STBTT_DEF extern
#include "stb_truetype.h"

struct FONSttFontImpl {
	stbtt_fontinfo font;
	int minAsciiGlyphIndex;
	int maxAsciiGlyphIndex;

#if FONS_ASCII_TO_GLYPH_INDEX_ARRAY
	int ascii_to_glyph_index[FONS_NUM_ASCII_TO_GLYPH_INDICES];
#endif
#if FONS_GLYPH_KERN_NONZERO_CODEMAP
	// MC: Here I do an attempt to mark for which combination of glyph indices
	// there exists a non-zero kerning value. Every slot in this array is 2 bit:
	//  00 - not looked up yet.
	//  01 - known to be zero
	//  10 - known to be non-zero
	//  11 - [unused]
	uint64_t *kern_codemap;

#endif
#if FONS_GLYPH_KERN_ARRAY_ASCII
	// MC: We will only store kerning information for the ASCII-ASCII
	// glyph combinations, assuming they will make up 99% of all character
	// sequences. However, we need to keep track of a table that can tell us
	// which ASCII character belongs to a glyph index, to be able to compatcly
	// store the lookup table, as glyphs for the ASCII characters can still
	// have very high indices (so we want to bring them back to a known small
	// range, which will determine the size of the lookup table).
	int16_t *kern_ascii;
	uint8_t *glyph_index_to_ascii; // Contains zero if the glyph is non-ASCII.
#endif
};
typedef struct FONSttFontImpl FONSttFontImpl;

int fons__tt_init(FONScontext *context)
{
	FONS_NOTUSED(context);
	return 1;
}

int fons__tt_getGlyphIndex(FONSttFontImpl *font, int codepoint)
{
#if FONS_ASCII_TO_GLYPH_INDEX_ARRAY
	if (codepoint >= FONS_FIRST_ASCII_CODEPOINT && codepoint <= FONS_LAST_ASCII_CODEPOINT) {
		return font->ascii_to_glyph_index[codepoint - FONS_FIRST_ASCII_CODEPOINT];
	}
#endif

	return stbtt_FindGlyphIndex(&font->font, codepoint);
}

int fons__tt_getGlyphKernAdvance(FONSttFontImpl *font, int glyph1, int glyph2)
{
#if FONS_GLYPH_KERN_ARRAY_ASCII || FONS_GLYPH_KERN_NONZERO_CODEMAP
	const int minID = font->minAsciiGlyphIndex;
	const int maxID = font->maxAsciiGlyphIndex;
	if(glyph1 >= minID && glyph1 <= maxID && glyph2 >= minID && glyph2 <= maxID) {
		const int g1 = glyph1 - minID;
		const int g2 = glyph2 - minID;

#if FONS_GLYPH_KERN_ARRAY_ASCII
		uint8_t ascii_codepoint_1 = font->glyph_index_to_ascii[g1];
		uint8_t ascii_codepoint_2 = font->glyph_index_to_ascii[g2];
		if (ascii_codepoint_1 != 0 && ascii_codepoint_2 != 0) {
			int32_t i1 = ascii_codepoint_1 - FONS_FIRST_ASCII_CODEPOINT;
			int32_t i2 = ascii_codepoint_2 - FONS_FIRST_ASCII_CODEPOINT;
			//// MC: Implement Rosenberg-Strong's pairing function to look up
			//// indices to compact memory usage for fonts with a lot of glyphs.
			const int max = bx::max(i1, i2);
			const int combo = max * max + max + i1 - i2;
			int16_t kern = font->kern_ascii[combo];
			if (kern == INT16_MIN) {
				kern = stbtt_GetGlyphKernAdvance(&font->font, glyph1, glyph2);
				font->kern_ascii[combo] = kern;
			}
			return kern;
		}
#endif

#if FONS_GLYPH_KERN_NONZERO_CODEMAP
		const int combo = g1 + g2 * (maxID - minID + 1);
		int32_t uint64_idx = (combo * 2) / (sizeof(uint64_t) * 8);
		int32_t bit_idx = (combo * 2) % (sizeof(uint64_t) * 8);

		uint64_t uint64_entry = font->kern_codemap[uint64_idx];
		int8_t status = (uint64_entry >> bit_idx) & 3;
		if (status == 0) {
			// Unknown
			int kern = stbtt_GetGlyphKernAdvance(&font->font, glyph1, glyph2);
			if (kern == 0) {
				// Update to known-to-be-zero
				uint64_entry |= 1 << bit_idx;
			} else {
				// Update to known-to-be-non-zero
				uint64_entry |= 2 << bit_idx;
			}
			font->kern_codemap[uint64_idx] = uint64_entry;
			return kern;
		} else if (status == 1) {
			// Known to be zero
			return 0;
		} else {
			// Known to be non-zero
			return stbtt_GetGlyphKernAdvance(&font->font, glyph1, glyph2);
		}
#endif
	}
#endif

	return stbtt_GetGlyphKernAdvance(&font->font, glyph1, glyph2);
}


int fons__tt_loadFont(FONScontext *context, FONSttFontImpl *font, unsigned char *data, int dataSize)
{
	int stbError;
	FONS_NOTUSED(dataSize);

	font->font.userdata = context;
	stbError = stbtt_InitFont(&font->font, data, 0);

	if (stbError) {
#if FONS_ASCII_TO_GLYPH_INDEX_ARRAY
		int minAsciiGlyphIndex = INT_MAX;
		int maxAsciiGlyphIndex = INT_MIN;
		for (int cp = FONS_FIRST_ASCII_CODEPOINT; cp <= FONS_LAST_ASCII_CODEPOINT; ++cp) {
			const int gi = stbtt_FindGlyphIndex(&font->font, cp);
			font->ascii_to_glyph_index[cp - FONS_FIRST_ASCII_CODEPOINT] = gi;

			if (gi < minAsciiGlyphIndex) {
				minAsciiGlyphIndex = gi;
			}
			if (gi > maxAsciiGlyphIndex) {
				maxAsciiGlyphIndex = gi;
			}
		}
		font->minAsciiGlyphIndex = minAsciiGlyphIndex;
		font->maxAsciiGlyphIndex = maxAsciiGlyphIndex;
#endif

#if FONS_GLYPH_KERN_ARRAY_ASCII || FONS_GLYPH_KERN_NONZERO_CODEMAP

		const int rangeGlyphIndicesAscii = maxAsciiGlyphIndex - minAsciiGlyphIndex + 1;
#if FONS_GLYPH_KERN_ARRAY_ASCII
		const int totalAsciiPairs = FONS_NUM_ASCII_TO_GLYPH_INDICES * FONS_NUM_ASCII_TO_GLYPH_INDICES;
		font->kern_ascii = (int16_t*)FONSmalloc(sizeof(int16_t) * totalAsciiPairs);
		font->glyph_index_to_ascii = (uint8_t*)FONSmalloc(sizeof(uint8_t) * rangeGlyphIndicesAscii);
#endif
#if FONS_GLYPH_KERN_NONZERO_CODEMAP
		const int totalPairs = rangeGlyphIndicesAscii * rangeGlyphIndicesAscii;
		const int totalBits = totalPairs * 2;
		const int totalBytes = (totalBits + 7) / 8;
		const int totalUint64s = (totalBytes + 7) / 8;
		font->kern_codemap = (uint64_t*)FONSmalloc(totalUint64s * sizeof(uint64_t));
#endif
		if (true
#if FONS_GLYPH_KERN_ARRAY_ASCII
				&& (font->kern_ascii && font->glyph_index_to_ascii)
#endif
#if FONS_GLYPH_KERN_NONZERO_CODEMAP
				&& (font->kern_codemap)
#endif
			 ) {
#if FONS_GLYPH_KERN_ARRAY_ASCII
			// MC: Clear the array the dummy value meaning the value is not cached yet.
			for (int i = 0; i < totalAsciiPairs; ++i) {
				font->kern_ascii[i] = INT16_MIN;
			}
			for (int i = 0; i < rangeGlyphIndicesAscii; ++i) {
				font->glyph_index_to_ascii[i] = 0;
			}
			for (int cp = FONS_FIRST_ASCII_CODEPOINT; cp <= FONS_LAST_ASCII_CODEPOINT; ++cp) {
				int glyphIdx = fons__tt_getGlyphIndex(font, cp);
				font->glyph_index_to_ascii[glyphIdx - font->minAsciiGlyphIndex] = cp;
			}
#endif
#if FONS_GLYPH_KERN_NONZERO_CODEMAP
			for (int i = 0; i < totalUint64s; ++i) {
				font->kern_codemap[i] = 0;
			}
#endif

			// MC: Pre-cache the kerning for ASCII characters.
			int min_kern = INT_MAX;
			int max_kern = INT_MIN;
			int non_zero_kern = 0;
			for (int second_cp = FONS_FIRST_ASCII_CODEPOINT; second_cp <= FONS_LAST_ASCII_CODEPOINT; ++second_cp) {
				int second = fons__tt_getGlyphIndex(font, second_cp);
				for (int first_cp = FONS_FIRST_ASCII_CODEPOINT; first_cp <= FONS_LAST_ASCII_CODEPOINT; ++first_cp) {
					int first = fons__tt_getGlyphIndex(font, first_cp);
					// MC: Do a lookup, which will fetch and cache the result.
					int value = fons__tt_getGlyphKernAdvance(font, first, second);

					min_kern = bx::min(min_kern, value);
					max_kern = bx::max(max_kern, value);
					non_zero_kern += (value != 0);
				}
			}
			assert(min_kern >= INT16_MIN);
			assert(max_kern <= INT16_MAX);
		}
#endif
	}

	return stbError;
}

void fons__tt_getFontVMetrics(FONSttFontImpl *font, int *ascent, int *descent, int *lineGap)
{
	stbtt_GetFontVMetrics(&font->font, ascent, descent, lineGap);
}

float fons__tt_getPixelHeightScale(FONSttFontImpl *font, float size)
{
	return stbtt_ScaleForPixelHeight(&font->font, size);
}
int fons__tt_buildGlyphBitmap(FONSttFontImpl *font, FONSttFontImpl* baseFont, int glyph, float size, float scale,
							  int *advance, int *lsb, int *x0, int *y0, int *x1, int *y1)
{
	FONS_NOTUSED(size);
	FONS_NOTUSED(baseFont);
	stbtt_GetGlyphHMetrics(&font->font, glyph, advance, lsb);
	stbtt_GetGlyphBitmapBox(&font->font, glyph, scale, scale, x0, y0, x1, y1);
	return 1;
}

void fons__tt_renderGlyphBitmap(FONSttFontImpl *font, unsigned char *output, int outWidth, int outHeight, int outStride,
								float scaleX, float scaleY, int glyph)
{
	stbtt_MakeGlyphBitmap(&font->font, output, outWidth, outHeight, outStride, scaleX, scaleY, glyph);
}

#endif

#ifndef FONS_SCRATCH_BUF_SIZE
#	define FONS_SCRATCH_BUF_SIZE 8 // JD: BGFX version of FontStash doesn't use the scratch buffer.
#endif
#ifndef FONS_HASH_LUT_SIZE
#	define FONS_HASH_LUT_SIZE 256
#endif
#ifndef FONS_INIT_FONTS
#	define FONS_INIT_FONTS 4
#endif
#ifndef FONS_INIT_GLYPHS
#	define FONS_INIT_GLYPHS 256
#endif
#ifndef FONS_INIT_ATLAS_NODES
#	define FONS_INIT_ATLAS_NODES 256
#endif
#ifndef FONS_VERTEX_COUNT
#	define FONS_VERTEX_COUNT 1024
#endif
#ifndef FONS_MAX_STATES
#	define FONS_MAX_STATES 20
#endif
#ifndef FONS_MAX_FALLBACKS
#	define FONS_MAX_FALLBACKS 20
#endif

#if FONS_SEPARATE_CODEPOINT
static unsigned int fons__hashint(unsigned int a)
{
	a += ~(a<<15);
	a ^=  (a>>10);
	a +=  (a<<3);
	a ^=  (a>>6);
	a += ~(a<<11);
	a ^=  (a>>16);
	return a;
}
#else
static unsigned int fons__hashGlyphCode(uint64_t glyphCode)
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
#endif

static int fons__mini(int a, int b)
{
	return a < b ? a : b;
}

static int fons__maxi(int a, int b)
{
	return a > b ? a : b;
}

struct FONSglyph
{
#if FONS_SEPARATE_CODEPOINT
	unsigned int codepoint;
	short size, blur;
#else
	uint64_t glyphCode;
#endif
	int next;
	int index;
	short x0,y0,x1,y1;
	short xadv,xoff,yoff;
};
typedef struct FONSglyph FONSglyph;

struct FONSfont
{
	FONSttFontImpl font;
	char name[64];
	unsigned char* data;
	int dataSize;
	unsigned char freeData;
	float ascender;
	float descender;
	float lineh;
	FONSglyph* glyphs;
	int cglyphs;
	int nglyphs;
	int lut[FONS_HASH_LUT_SIZE];
	int fallbacks[FONS_MAX_FALLBACKS];
	int nfallbacks;
};
typedef struct FONSfont FONSfont;

struct FONSstate
{
	int font;
	int align;
	float size;
	unsigned int color;
	float blur;
	float spacing;
};
typedef struct FONSstate FONSstate;

struct FONSatlasNode {
    short x, y, width;
};
typedef struct FONSatlasNode FONSatlasNode;

struct FONSatlas
{
	int width, height;
	FONSatlasNode* nodes;
	int nnodes;
	int cnodes;
};
typedef struct FONSatlas FONSatlas;

struct FONScontext
{
	FONSparams params;
	float itw,ith;
	unsigned char* texData;
	int dirtyRect[4];
	FONSfont** fonts;
	FONSatlas* atlas;
	int cfonts;
	int nfonts;
#if FONS_RENDERING
	float verts[FONS_VERTEX_COUNT*2];
	float tcoords[FONS_VERTEX_COUNT*2];
	unsigned int colors[FONS_VERTEX_COUNT];
	int nverts;
#endif
	unsigned char* scratch;
	int nscratch;
	FONSstate states[FONS_MAX_STATES];
	int nstates;
	void (*handleError)(void* uptr, int error, int val);
	void* errorUptr;
	int atlasID; // JD: Counts how many times the atlas has been reset. Used for FONSstring baking.
};

#if 0 // defined(STB_TRUETYPE_IMPLEMENTATION)

static void* fons__tmpalloc(size_t size, void* up)
{
	unsigned char* ptr;
	FONScontext* stash = (FONScontext*)up;

	// 16-byte align the returned pointer
	size = (size + 0xf) & ~0xf;

	if (stash->nscratch+(int)size > FONS_SCRATCH_BUF_SIZE) {
		if (stash->handleError)
			stash->handleError(stash->errorUptr, FONS_SCRATCH_FULL, stash->nscratch+(int)size);
		return NULL;
	}
	ptr = stash->scratch + stash->nscratch;
	stash->nscratch += (int)size;
	return ptr;
}

static void fons__tmpfree(void* ptr, void* up)
{
	(void)ptr;
	(void)up;
	// empty
}

#endif // STB_TRUETYPE_IMPLEMENTATION

// Copyright (c) 2008-2010 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.

#define FONS_UTF8_ACCEPT 0
#define FONS_UTF8_REJECT 12

static unsigned int fons__decutf8(unsigned int* state, unsigned int* codep, unsigned int byte)
{
	static const unsigned char utf8d[] = {
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

	unsigned int type = utf8d[byte];

    *codep = (*state != FONS_UTF8_ACCEPT) ?
		(byte & 0x3fu) | (*codep << 6) :
		(0xff >> type) & (byte);

	*state = utf8d[256 + *state + type];
	return *state;
}

// Atlas based on Skyline Bin Packer by Jukka Jylänki

static void fons__deleteAtlas(FONSatlas* atlas)
{
	if (atlas == NULL) return;
	if (atlas->nodes != NULL) FONSfree(atlas->nodes);
	FONSfree(atlas);
}

static FONSatlas* fons__allocAtlas(int w, int h, int nnodes)
{
	FONSatlas* atlas = NULL;

	// Allocate memory for the font stash.
	atlas = (FONSatlas*)FONSmalloc(sizeof(FONSatlas));
	if (atlas == NULL) goto error;
	memset(atlas, 0, sizeof(FONSatlas));

	atlas->width = w;
	atlas->height = h;

	// Allocate space for skyline nodes
	atlas->nodes = (FONSatlasNode*)FONSmalloc(sizeof(FONSatlasNode) * nnodes);
	if (atlas->nodes == NULL) goto error;
	memset(atlas->nodes, 0, sizeof(FONSatlasNode) * nnodes);
	atlas->nnodes = 0;
	atlas->cnodes = nnodes;

	// Init root node.
	atlas->nodes[0].x = 0;
	atlas->nodes[0].y = 0;
	atlas->nodes[0].width = (short)w;
	atlas->nnodes++;

	return atlas;

error:
	if (atlas) fons__deleteAtlas(atlas);
	return NULL;
}

static int fons__atlasInsertNode(FONSatlas* atlas, int idx, int x, int y, int w)
{
	int i;
	// Insert node
	if (atlas->nnodes+1 > atlas->cnodes) {
		atlas->cnodes = atlas->cnodes == 0 ? 8 : atlas->cnodes * 2;
		atlas->nodes = (FONSatlasNode*)FONSrealloc(atlas->nodes, sizeof(FONSatlasNode) * atlas->cnodes);
		if (atlas->nodes == NULL)
			return 0;
	}
	for (i = atlas->nnodes; i > idx; i--)
		atlas->nodes[i] = atlas->nodes[i-1];
	atlas->nodes[idx].x = (short)x;
	atlas->nodes[idx].y = (short)y;
	atlas->nodes[idx].width = (short)w;
	atlas->nnodes++;

	return 1;
}

static void fons__atlasRemoveNode(FONSatlas* atlas, int idx)
{
	int i;
	if (atlas->nnodes == 0) return;
	for (i = idx; i < atlas->nnodes-1; i++)
		atlas->nodes[i] = atlas->nodes[i+1];
	atlas->nnodes--;
}

static void fons__atlasExpand(FONSatlas* atlas, int w, int h)
{
	// Insert node for empty space
	if (w > atlas->width)
		fons__atlasInsertNode(atlas, atlas->nnodes, atlas->width, 0, w - atlas->width);
	atlas->width = w;
	atlas->height = h;
}

static void fons__atlasReset(FONSatlas* atlas, int w, int h)
{
	atlas->width = w;
	atlas->height = h;
	atlas->nnodes = 0;

	// Init root node.
	atlas->nodes[0].x = 0;
	atlas->nodes[0].y = 0;
	atlas->nodes[0].width = (short)w;
	atlas->nnodes++;
}

static int fons__atlasAddSkylineLevel(FONSatlas* atlas, int idx, int x, int y, int w, int h)
{
	int i;

	// Insert new node
	if (fons__atlasInsertNode(atlas, idx, x, y+h, w) == 0)
		return 0;

	// Delete skyline segments that fall under the shadow of the new segment.
	for (i = idx+1; i < atlas->nnodes; i++) {
		if (atlas->nodes[i].x < atlas->nodes[i-1].x + atlas->nodes[i-1].width) {
			int shrink = atlas->nodes[i-1].x + atlas->nodes[i-1].width - atlas->nodes[i].x;
			atlas->nodes[i].x += (short)shrink;
			atlas->nodes[i].width -= (short)shrink;
			if (atlas->nodes[i].width <= 0) {
				fons__atlasRemoveNode(atlas, i);
				i--;
			} else {
				break;
			}
		} else {
			break;
		}
	}

	// Merge same height skyline segments that are next to each other.
	for (i = 0; i < atlas->nnodes-1; i++) {
		if (atlas->nodes[i].y == atlas->nodes[i+1].y) {
			atlas->nodes[i].width += atlas->nodes[i+1].width;
			fons__atlasRemoveNode(atlas, i+1);
			i--;
		}
	}

	return 1;
}

static int fons__atlasRectFits(FONSatlas* atlas, int i, int w, int h)
{
	// Checks if there is enough space at the location of skyline span 'i',
	// and return the max height of all skyline spans under that at that location,
	// (think tetris block being dropped at that position). Or -1 if no space found.
	int x = atlas->nodes[i].x;
	int y = atlas->nodes[i].y;
	int spaceLeft;
	if (x + w > atlas->width)
		return -1;
	spaceLeft = w;
	while (spaceLeft > 0) {
		if (i == atlas->nnodes) return -1;
		y = fons__maxi(y, atlas->nodes[i].y);
		if (y + h > atlas->height) return -1;
		spaceLeft -= atlas->nodes[i].width;
		++i;
	}
	return y;
}

static int fons__atlasAddRect(FONSatlas* atlas, int rw, int rh, int* rx, int* ry)
{
	int besth = atlas->height, bestw = atlas->width, besti = -1;
	int bestx = -1, besty = -1, i;

	// Bottom left fit heuristic.
	for (i = 0; i < atlas->nnodes; i++) {
		int y = fons__atlasRectFits(atlas, i, rw, rh);
		if (y != -1) {
			if (y + rh < besth || (y + rh == besth && atlas->nodes[i].width < bestw)) {
				besti = i;
				bestw = atlas->nodes[i].width;
				besth = y + rh;
				bestx = atlas->nodes[i].x;
				besty = y;
			}
		}
	}

	if (besti == -1)
		return 0;

	// Perform the actual packing.
	if (fons__atlasAddSkylineLevel(atlas, besti, bestx, besty, rw, rh) == 0)
		return 0;

	*rx = bestx;
	*ry = besty;

	return 1;
}

static void fons__addWhiteRect(FONScontext* stash, int w, int h)
{
	int x, y, gx, gy;
	unsigned char* dst;
	if (fons__atlasAddRect(stash->atlas, w, h, &gx, &gy) == 0)
		return;

	// Rasterize
	dst = &stash->texData[gx + gy * stash->params.width];
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++)
			dst[x] = 0xff;
		dst += stash->params.width;
	}

	stash->dirtyRect[0] = fons__mini(stash->dirtyRect[0], gx);
	stash->dirtyRect[1] = fons__mini(stash->dirtyRect[1], gy);
	stash->dirtyRect[2] = fons__maxi(stash->dirtyRect[2], gx+w);
	stash->dirtyRect[3] = fons__maxi(stash->dirtyRect[3], gy+h);
}

FONScontext* fonsCreateInternal(FONSparams* params)
{
	FONScontext* stash = NULL;

#if FONS_CUSTOM_WHITE_RECT
	const int wrw = params->whiteRectWidth <= 0 ? 2 : params->whiteRectWidth;
	const int wrh = params->whiteRectHeight <= 0 ? 2 : params->whiteRectHeight;
#else
	const int wrw = 2;
	const int wrh = 2;
#endif

	// Allocate memory for the font stash.
	stash = (FONScontext*)FONSmalloc(sizeof(FONScontext));
	if (stash == NULL) goto error;
	memset(stash, 0, sizeof(FONScontext));

	stash->atlasID = 1;
	stash->params = *params;

	// Allocate scratch buffer.
	stash->scratch = (unsigned char*)FONSmalloc(FONS_SCRATCH_BUF_SIZE);
	if (stash->scratch == NULL) goto error;

	// Initialize implementation library
	if (!fons__tt_init(stash)) goto error;

	if (stash->params.renderCreate != NULL) {
		if (stash->params.renderCreate(stash->params.userPtr, stash->params.width, stash->params.height) == 0)
			goto error;
	}

	stash->atlas = fons__allocAtlas(stash->params.width, stash->params.height, FONS_INIT_ATLAS_NODES);
	if (stash->atlas == NULL) goto error;

	// Allocate space for fonts.
	stash->fonts = (FONSfont**)FONSmalloc(sizeof(FONSfont*) * FONS_INIT_FONTS);
	if (stash->fonts == NULL) goto error;
	memset(stash->fonts, 0, sizeof(FONSfont*) * FONS_INIT_FONTS);
	stash->cfonts = FONS_INIT_FONTS;
	stash->nfonts = 0;

	// Create texture for the cache.
	stash->itw = 1.0f/stash->params.width;
	stash->ith = 1.0f/stash->params.height;
	stash->texData = (unsigned char*)FONSmalloc(stash->params.width * stash->params.height);
	if (stash->texData == NULL) goto error;
	memset(stash->texData, 0, stash->params.width * stash->params.height);

	stash->dirtyRect[0] = stash->params.width;
	stash->dirtyRect[1] = stash->params.height;
	stash->dirtyRect[2] = 0;
	stash->dirtyRect[3] = 0;

	// Add white rect at 0,0 for debug drawing.
	fons__addWhiteRect(stash, wrw, wrh);

	fonsPushState(stash);
	fonsClearState(stash);

	return stash;

error:
	fonsDeleteInternal(stash);
	return NULL;
}

static FONSstate* fons__getState(FONScontext* stash)
{
	return &stash->states[stash->nstates-1];
}

int fonsAddFallbackFont(FONScontext* stash, int base, int fallback)
{
	FONSfont* baseFont = stash->fonts[base];
	if (baseFont->nfallbacks < FONS_MAX_FALLBACKS) {
		baseFont->fallbacks[baseFont->nfallbacks++] = fallback;
		return 1;
	}
	return 0;
}

void fonsSetSize(FONScontext* stash, float size)
{
	fons__getState(stash)->size = size;
}

void fonsSetColor(FONScontext* stash, unsigned int color)
{
	fons__getState(stash)->color = color;
}

void fonsSetSpacing(FONScontext* stash, float spacing)
{
	fons__getState(stash)->spacing = spacing;
}

void fonsSetBlur(FONScontext* stash, float blur)
{
	fons__getState(stash)->blur = blur;
}

void fonsSetAlign(FONScontext* stash, int align)
{
	fons__getState(stash)->align = align;
}

void fonsSetFont(FONScontext* stash, int font)
{
	fons__getState(stash)->font = font;
}

void fonsPushState(FONScontext* stash)
{
	if (stash->nstates >= FONS_MAX_STATES) {
		if (stash->handleError)
			stash->handleError(stash->errorUptr, FONS_STATES_OVERFLOW, 0);
		return;
	}
	if (stash->nstates > 0)
		memcpy(&stash->states[stash->nstates], &stash->states[stash->nstates-1], sizeof(FONSstate));
	stash->nstates++;
}

void fonsPopState(FONScontext* stash)
{
	if (stash->nstates <= 1) {
		if (stash->handleError)
			stash->handleError(stash->errorUptr, FONS_STATES_UNDERFLOW, 0);
		return;
	}
	stash->nstates--;
}

void fonsClearState(FONScontext* stash)
{
	FONSstate* state = fons__getState(stash);
	state->size = 12.0f;
	state->color = 0xffffffff;
	state->font = 0;
	state->blur = 0;
	state->spacing = 0;
	state->align = FONS_ALIGN_LEFT | FONS_ALIGN_BASELINE;
}

static void fons__freeFont(FONSfont* font)
{
	if (font == NULL) return;
	if (font->glyphs) FONSfree(font->glyphs);
	if (font->freeData && font->data) FONSfree(font->data);
	FONSfree(font);
}

static int fons__allocFont(FONScontext* stash)
{
	FONSfont* font = NULL;
	if (stash->nfonts+1 > stash->cfonts) {
		stash->cfonts = stash->cfonts == 0 ? 8 : stash->cfonts * 2;
		stash->fonts = (FONSfont**)FONSrealloc(stash->fonts, sizeof(FONSfont*) * stash->cfonts);
		if (stash->fonts == NULL)
			return -1;
	}
	font = (FONSfont*)FONSmalloc(sizeof(FONSfont));
	if (font == NULL) goto error;
	memset(font, 0, sizeof(FONSfont));

	font->glyphs = (FONSglyph*)FONSmalloc(sizeof(FONSglyph) * FONS_INIT_GLYPHS);
	if (font->glyphs == NULL) goto error;
	font->cglyphs = FONS_INIT_GLYPHS;
	font->nglyphs = 0;

	stash->fonts[stash->nfonts++] = font;
	return stash->nfonts-1;

error:
	fons__freeFont(font);

	return FONS_INVALID;
}

#if FONS_STDIO
int fonsAddFont(FONScontext* stash, const char* name, const char* path)
{
	FILE* fp = 0;
	int dataSize = 0;
	unsigned char* data = NULL;

	// Read in the font data.
	fp = fopen(path, "rb");
	if (fp == NULL) goto error;
	fseek(fp,0,SEEK_END);
	dataSize = (int)ftell(fp);
	fseek(fp,0,SEEK_SET);
	data = (unsigned char*)FONSmalloc(dataSize);
	if (data == NULL) goto error;
	fread(data, 1, dataSize, fp);
	fclose(fp);
	fp = 0;

	return fonsAddFontMem(stash, name, data, dataSize, 1);

error:
	if (data) FONSfree(data);
	if (fp) fclose(fp);
	return FONS_INVALID;
}
#endif

int fonsAddFontMem(FONScontext* stash, const char* name, unsigned char* data, int dataSize, int freeData)
{
	int i, ascent, descent, fh, lineGap;
	FONSfont* font;

	int idx = fons__allocFont(stash);
	if (idx == FONS_INVALID)
		return FONS_INVALID;

	font = stash->fonts[idx];

	strncpy(font->name, name, sizeof(font->name));
	font->name[sizeof(font->name)-1] = '\0';

	// Init hash lookup.
	for (i = 0; i < FONS_HASH_LUT_SIZE; ++i) {
		font->lut[i] = -1;
	}

	// Read in the font data.
	font->dataSize = dataSize;
	font->data = data;
	font->freeData = (unsigned char)freeData;

	// Init font
	stash->nscratch = 0;
	if (!fons__tt_loadFont(stash, &font->font, data, dataSize)) goto error;

	// Store normalized line height. The real line height is got
	// by multiplying the lineh by font size.
	fons__tt_getFontVMetrics( &font->font, &ascent, &descent, &lineGap);
	fh = ascent - descent;
	font->ascender = (float)ascent / (float)fh;
	font->descender = (float)descent / (float)fh;
	font->lineh = (float)(fh + lineGap) / (float)fh;

	return idx;

error:
	fons__freeFont(font);
	stash->nfonts--;
	return FONS_INVALID;
}

int fonsGetFontByName(FONScontext* s, const char* name)
{
	int i;
	for (i = 0; i < s->nfonts; i++) {
		if (strcmp(s->fonts[i]->name, name) == 0)
			return i;
	}
	return FONS_INVALID;
}


static FONSglyph* fons__allocGlyph(FONSfont* font)
{
	if (font->nglyphs+1 > font->cglyphs) {
		font->cglyphs = font->cglyphs == 0 ? 8 : font->cglyphs * 2;
		font->glyphs = (FONSglyph*)FONSrealloc(font->glyphs, sizeof(FONSglyph) * font->cglyphs);
		if (font->glyphs == NULL) return NULL;
	}
	font->nglyphs++;
	return &font->glyphs[font->nglyphs-1];
}


// Based on Exponential blur, Jani Huhtanen, 2006

#define APREC 16
#define ZPREC 7

static void fons__blurCols(unsigned char* dst, int w, int h, int dstStride, int alpha)
{
	int x, y;
	for (y = 0; y < h; y++) {
		int z = 0; // force zero border
		for (x = 1; x < w; x++) {
			z += (alpha * (((int)(dst[x]) << ZPREC) - z)) >> APREC;
			dst[x] = (unsigned char)(z >> ZPREC);
		}
		dst[w-1] = 0; // force zero border
		z = 0;
		for (x = w-2; x >= 0; x--) {
			z += (alpha * (((int)(dst[x]) << ZPREC) - z)) >> APREC;
			dst[x] = (unsigned char)(z >> ZPREC);
		}
		dst[0] = 0; // force zero border
		dst += dstStride;
	}
}

static void fons__blurRows(unsigned char* dst, int w, int h, int dstStride, int alpha)
{
	int x, y;
	for (x = 0; x < w; x++) {
		int z = 0; // force zero border
		for (y = dstStride; y < h*dstStride; y += dstStride) {
			z += (alpha * (((int)(dst[y]) << ZPREC) - z)) >> APREC;
			dst[y] = (unsigned char)(z >> ZPREC);
		}
		dst[(h-1)*dstStride] = 0; // force zero border
		z = 0;
		for (y = (h-2)*dstStride; y >= 0; y -= dstStride) {
			z += (alpha * (((int)(dst[y]) << ZPREC) - z)) >> APREC;
			dst[y] = (unsigned char)(z >> ZPREC);
		}
		dst[0] = 0; // force zero border
		dst++;
	}
}


static void fons__blur(FONScontext* stash, unsigned char* dst, int w, int h, int dstStride, int blur)
{
	int alpha;
	float sigma;
	(void)stash;

	if (blur < 1)
		return;
	// Calculate the alpha such that 90% of the kernel is within the radius. (Kernel extends to infinity)
	sigma = (float)blur * 0.57735f; // 1 / sqrt(3)
	alpha = (int)((1<<APREC) * (1.0f - bx::exp(-2.3f / (sigma+1.0f))));
	fons__blurRows(dst, w, h, dstStride, alpha);
	fons__blurCols(dst, w, h, dstStride, alpha);
	fons__blurRows(dst, w, h, dstStride, alpha);
	fons__blurCols(dst, w, h, dstStride, alpha);
//	fons__blurrows(dst, w, h, dstStride, alpha);
//	fons__blurcols(dst, w, h, dstStride, alpha);
}

static FONSglyph* fons__getGlyph(FONScontext* stash, FONSfont* font, unsigned int codepoint,
								 short isize, short iblur, int bitmapOption)
{
	int i, g, advance, lsb, x0, y0, x1, y1, gw, gh, gx, gy, x, y;
	float scale;
	FONSglyph* glyph = NULL;
	unsigned int h;
	float size = isize/10.0f;
	int pad, added;
	unsigned char* bdst;
	unsigned char* dst;
	FONSfont* renderFont = font;

	if (isize < 2) return NULL;
	if (iblur > 20) iblur = 20;
	pad = iblur+2;

	// Reset allocator.
	stash->nscratch = 0;

	// Find code point and size.
#if FONS_SEPARATE_CODEPOINT
	h = fons__hashint(codepoint) & (FONS_HASH_LUT_SIZE-1);
#else
	const uint64_t glyphCode = MAKE_GLYPH_CODE(codepoint, isize, iblur);
	h = fons__hashGlyphCode(glyphCode) & (FONS_HASH_LUT_SIZE - 1);
#endif

	// TODO: JD: This loop is the hottest part of the function under normal usage (i.e. all required glyphs are already cached).
	// The alternative hash function helps with glyph distribution in the LUT but we still have enough collisions to make the
	// loop execute multiple times per call. Larger LUT doesn't seem to help (in fact they seem to hurt perf).
	// It might help if the jumping around in memory from glyph to glyph was avoided (i.e. glyphs in the same bucket are
	// stored sequentially in the glyphs array) (No it doesn't!).
	i = font->lut[h];
	while (i != -1) {
#if FONS_SEPARATE_CODEPOINT
		if (font->glyphs[i].codepoint == codepoint && font->glyphs[i].size == isize && font->glyphs[i].blur == iblur) {
			glyph = &font->glyphs[i];
			if (bitmapOption == FONS_GLYPH_BITMAP_OPTIONAL || (glyph->x0 >= 0 && glyph->y0 >= 0)) {
				return glyph;
			}

			// At this point, glyph exists but the bitmap data is not yet created.
			break;
		}
#else
		if (font->glyphs[i].glyphCode == glyphCode) {
			glyph = &font->glyphs[i];
			if (bitmapOption == FONS_GLYPH_BITMAP_OPTIONAL || (glyph->x0 >= 0 && glyph->y0 >= 0)) {
				return glyph;
			}

			// At this point, glyph exists but the bitmap data is not yet created.
			break;
		}
#endif
		i = font->glyphs[i].next;
	}

	// Could not find glyph, create it.
	g = fons__tt_getGlyphIndex(&font->font, codepoint);
	// Try to find the glyph in fallback fonts.
	if (g == 0) {
		for (i = 0; i < font->nfallbacks; ++i) {
			FONSfont* fallbackFont = stash->fonts[font->fallbacks[i]];
			int fallbackIndex = fons__tt_getGlyphIndex(&fallbackFont->font, codepoint);
			if (fallbackIndex != 0) {
				g = fallbackIndex;
				renderFont = fallbackFont;
				break;
			}
		}
		// It is possible that we did not find a fallback glyph.
		// In that case the glyph index 'g' is 0, and we'll proceed below and cache empty glyph.
	}
	scale = fons__tt_getPixelHeightScale(&renderFont->font, size);
	fons__tt_buildGlyphBitmap(&renderFont->font, &font->font, g, size, scale, &advance, &lsb, &x0, &y0, &x1, &y1);
	gw = x1-x0 + pad*2;
	gh = y1-y0 + pad*2;

	if (bitmapOption == FONS_GLYPH_BITMAP_REQUIRED) {
		// Find free spot for the rect in the atlas
		added = fons__atlasAddRect(stash->atlas, gw, gh, &gx, &gy);
		if (added == 0 && stash->handleError != NULL) {
			// Atlas is full, let the user to resize the atlas (or not), and try again.
			stash->handleError(stash->errorUptr, FONS_ATLAS_FULL, 0);
			added = fons__atlasAddRect(stash->atlas, gw, gh, &gx, &gy);
		}
		if (added == 0) return NULL;
	} else {
		gx = -1;
		gy = -1;
	}

	// Init glyph.
	if (glyph == NULL) {
		glyph = fons__allocGlyph(font);
#if FONS_SEPARATE_CODEPOINT
		glyph->codepoint = codepoint;
		glyph->size = isize;
		glyph->blur = iblur;
#else
		glyph->glyphCode = MAKE_GLYPH_CODE(codepoint, isize, iblur);
#endif

		// Insert char to hash lookup.
		glyph->next = font->lut[h];
		font->lut[h] = font->nglyphs - 1;
	}

	glyph->index = g;
	glyph->x0 = (short)gx;
	glyph->y0 = (short)gy;
	glyph->x1 = (short)(glyph->x0+gw);
	glyph->y1 = (short)(glyph->y0+gh);
	glyph->xadv = (short)(scale * advance * 10.0f);
	glyph->xoff = (short)(x0 - pad);
	glyph->yoff = (short)(y0 - pad);

	if (bitmapOption == FONS_GLYPH_BITMAP_OPTIONAL) {
		return glyph;
	}

	// Rasterize
	dst = &stash->texData[(glyph->x0+pad) + (glyph->y0+pad) * stash->params.width];
	fons__tt_renderGlyphBitmap(&renderFont->font, dst, gw-pad*2,gh-pad*2, stash->params.width, scale,scale, g);

	// Make sure there is one pixel empty border.
	dst = &stash->texData[glyph->x0 + glyph->y0 * stash->params.width];
	for (y = 0; y < gh; y++) {
		dst[y*stash->params.width] = 0;
		dst[gw-1 + y*stash->params.width] = 0;
	}
	for (x = 0; x < gw; x++) {
		dst[x] = 0;
		dst[x + (gh-1)*stash->params.width] = 0;
	}

	// Debug code to color the glyph background
/*	unsigned char* fdst = &stash->texData[glyph->x0 + glyph->y0 * stash->params.width];
	for (y = 0; y < gh; y++) {
		for (x = 0; x < gw; x++) {
			int a = (int)fdst[x+y*stash->params.width] + 20;
			if (a > 255) a = 255;
			fdst[x+y*stash->params.width] = a;
		}
	}*/

	// Blur
	if (iblur > 0) {
		stash->nscratch = 0;
		bdst = &stash->texData[glyph->x0 + glyph->y0 * stash->params.width];
		fons__blur(stash, bdst, gw,gh, stash->params.width, iblur);
	}

	stash->dirtyRect[0] = fons__mini(stash->dirtyRect[0], glyph->x0);
	stash->dirtyRect[1] = fons__mini(stash->dirtyRect[1], glyph->y0);
	stash->dirtyRect[2] = fons__maxi(stash->dirtyRect[2], glyph->x1);
	stash->dirtyRect[3] = fons__maxi(stash->dirtyRect[3], glyph->y1);

	return glyph;
}

static void fons__getQuad(FONScontext* stash, FONSfont* font,
						   int prevGlyphIndex, FONSglyph* glyph,
						   float scale, float spacing, float* x, float* y, FONSquad* q)
{
	float rx,ry,xoff,yoff,x0,y0,x1,y1;

	if (prevGlyphIndex != -1) {
		float adv = fons__tt_getGlyphKernAdvance(&font->font, prevGlyphIndex, glyph->index) * scale;
#if FONS_SNAP_TO_GRID
		*x += (int)(adv + spacing + 0.5f);
#else
		*x += adv + spacing;
#endif
	}

	// Each glyph has 2px border to allow good interpolation,
	// one pixel to prevent leaking, and one to allow good interpolation for rendering.
	// Inset the texture region by one pixel for correct interpolation.
	xoff = (short)(glyph->xoff+1);
	yoff = (short)(glyph->yoff+1);
	x0 = (float)(glyph->x0+1);
	y0 = (float)(glyph->y0+1);
	x1 = (float)(glyph->x1-1);
	y1 = (float)(glyph->y1-1);

	if (stash->params.flags & FONS_ZERO_TOPLEFT) {
#if FONS_SNAP_TO_GRID
		rx = (float)(int)(*x + xoff);
		ry = (float)(int)(*y + yoff);
#else
		rx = *x + xoff;
		ry = *y + yoff;
#endif

		q->x0 = rx;
		q->y0 = ry;
		q->x1 = rx + x1 - x0;
		q->y1 = ry + y1 - y0;

		q->s0 = x0 * stash->itw;
		q->t0 = y0 * stash->ith;
		q->s1 = x1 * stash->itw;
		q->t1 = y1 * stash->ith;
	} else {
#if FONS_SNAP_TO_GRID
		rx = (float)(int)(*x + xoff);
		ry = (float)(int)(*y - yoff);
#else
		rx = *x + xoff;
		ry = *y - yoff;
#endif

		q->x0 = rx;
		q->y0 = ry;
		q->x1 = rx + x1 - x0;
		q->y1 = ry - y1 + y0;

		q->s0 = x0 * stash->itw;
		q->t0 = y0 * stash->ith;
		q->s1 = x1 * stash->itw;
		q->t1 = y1 * stash->ith;
	}

#if FONS_SNAP_TO_GRID
	*x += (int)(glyph->xadv / 10.0f + 0.5f);
#else
	*x += glyph->xadv / 10.0f;
#endif
}

#if FONS_RENDERING
static void fons__flush(FONScontext* stash)
{
	// Flush texture
	if (stash->dirtyRect[0] < stash->dirtyRect[2] && stash->dirtyRect[1] < stash->dirtyRect[3]) {
		if (stash->params.renderUpdate != NULL)
			stash->params.renderUpdate(stash->params.userPtr, stash->dirtyRect, stash->texData);
		// Reset dirty rect
		stash->dirtyRect[0] = stash->params.width;
		stash->dirtyRect[1] = stash->params.height;
		stash->dirtyRect[2] = 0;
		stash->dirtyRect[3] = 0;
	}

	// Flush triangles
	if (stash->nverts > 0) {
		if (stash->params.renderDraw != NULL)
			stash->params.renderDraw(stash->params.userPtr, stash->verts, stash->tcoords, stash->colors, stash->nverts);
		stash->nverts = 0;
	}
}
#endif

#if FONS_RENDERING
static __inline void fons__vertex(FONScontext* stash, float x, float y, float s, float t, unsigned int c)
{
	stash->verts[stash->nverts*2+0] = x;
	stash->verts[stash->nverts*2+1] = y;
	stash->tcoords[stash->nverts*2+0] = s;
	stash->tcoords[stash->nverts*2+1] = t;
	stash->colors[stash->nverts] = c;
	stash->nverts++;
}
#endif

static float fons__getVertAlign(FONScontext* stash, FONSfont* font, int align, short isize)
{
	if (stash->params.flags & FONS_ZERO_TOPLEFT) {
		if (align & FONS_ALIGN_TOP) {
			return font->ascender * (float)isize/10.0f;
		} else if (align & FONS_ALIGN_MIDDLE) {
			return (font->ascender + font->descender) / 2.0f * (float)isize/10.0f;
		} else if (align & FONS_ALIGN_BASELINE) {
			return 0.0f;
		} else if (align & FONS_ALIGN_BOTTOM) {
			return font->descender * (float)isize/10.0f;
		}
	} else {
		if (align & FONS_ALIGN_TOP) {
			return -font->ascender * (float)isize/10.0f;
		} else if (align & FONS_ALIGN_MIDDLE) {
			return -(font->ascender + font->descender) / 2.0f * (float)isize/10.0f;
		} else if (align & FONS_ALIGN_BASELINE) {
			return 0.0f;
		} else if (align & FONS_ALIGN_BOTTOM) {
			return -font->descender * (float)isize/10.0f;
		}
	}
	return 0.0;
}

#if FONS_RENDERING
float fonsDrawText(FONScontext* stash,
				   float x, float y,
				   const char* str, const char* end)
{
	FONSstate* state = fons__getState(stash);
	unsigned int codepoint;
	unsigned int utf8state = 0;
	FONSglyph* glyph = NULL;
	FONSquad q;
	int prevGlyphIndex = -1;
	short isize = (short)(state->size*10.0f);
	short iblur = (short)state->blur;
	float scale;
	FONSfont* font;
	float width;

	if (stash == NULL) return x;
	if (state->font < 0 || state->font >= stash->nfonts) return x;
	font = stash->fonts[state->font];
	if (font->data == NULL) return x;

	scale = fons__tt_getPixelHeightScale(&font->font, (float)isize/10.0f);

	if (end == NULL)
		end = str + strlen(str);

	// Align horizontally
	if (state->align & FONS_ALIGN_LEFT) {
		// empty
	} else if (state->align & FONS_ALIGN_RIGHT) {
		width = fonsTextBounds(stash, x,y, str, end, NULL);
		x -= width;
	} else if (state->align & FONS_ALIGN_CENTER) {
		width = fonsTextBounds(stash, x,y, str, end, NULL);
		x -= width * 0.5f;
	}
	// Align vertically.
	y += fons__getVertAlign(stash, font, state->align, isize);

	for (; str != end; ++str) {
		if (fons__decutf8(&utf8state, &codepoint, *(const unsigned char*)str))
			continue;
		glyph = fons__getGlyph(stash, font, codepoint, isize, iblur, FONS_GLYPH_BITMAP_REQUIRED);
		if (glyph != NULL) {
			fons__getQuad(stash, font, prevGlyphIndex, glyph, scale, state->spacing, &x, &y, &q);

			if (stash->nverts+6 > FONS_VERTEX_COUNT)
				fons__flush(stash);

			fons__vertex(stash, q.x0, q.y0, q.s0, q.t0, state->color);
			fons__vertex(stash, q.x1, q.y1, q.s1, q.t1, state->color);
			fons__vertex(stash, q.x1, q.y0, q.s1, q.t0, state->color);

			fons__vertex(stash, q.x0, q.y0, q.s0, q.t0, state->color);
			fons__vertex(stash, q.x0, q.y1, q.s0, q.t1, state->color);
			fons__vertex(stash, q.x1, q.y1, q.s1, q.t1, state->color);
		}
		prevGlyphIndex = glyph != NULL ? glyph->index : -1;
	}
	fons__flush(stash);

	return x;
}
#endif

int fonsTextIterInit(FONScontext* stash, FONStextIter* iter,
					 float x, float y, const char* str, const char* end, int bitmapOption)
{
	if (stash == NULL) return 0;
	FONSstate* state = fons__getState(stash);
	float width;

	memset(iter, 0, sizeof(*iter));

	if (state->font < 0 || state->font >= stash->nfonts) return 0;
	iter->font = stash->fonts[state->font];
	if (iter->font->data == NULL) return 0;

	iter->isize = (short)(state->size*10.0f);
	iter->iblur = (short)state->blur;
	iter->scale = fons__tt_getPixelHeightScale(&iter->font->font, (float)iter->isize/10.0f);

	// Align horizontally
	if (state->align & FONS_ALIGN_LEFT) {
		// empty
	} else if (state->align & FONS_ALIGN_RIGHT) {
		width = fonsTextBounds(stash, x,y, str, end, NULL);
		x -= width;
	} else if (state->align & FONS_ALIGN_CENTER) {
		width = fonsTextBounds(stash, x,y, str, end, NULL);
		x -= width * 0.5f;
	}
	// Align vertically.
	y += fons__getVertAlign(stash, iter->font, state->align, iter->isize);

	if (end == NULL)
		end = str + strlen(str);

	iter->x = iter->nextx = x;
	iter->y = iter->nexty = y;
	iter->spacing = state->spacing;
	iter->str = str;
	iter->next = str;
	iter->end = end;
	iter->codepoint = 0;
	iter->prevGlyphIndex = -1;
	iter->bitmapOption = bitmapOption;

	return 1;
}

int fonsTextIterNext(FONScontext* stash, FONStextIter* iter, FONSquad* quad)
{
	FONSglyph* glyph = NULL;
	const char* str = iter->next;
	iter->str = iter->next;

	if (str == iter->end)
		return 0;

	for (; str != iter->end; str++) {
		if (fons__decutf8(&iter->utf8state, &iter->codepoint, *(const unsigned char*)str))
			continue;
		str++;
		// Get glyph and quad
		iter->x = iter->nextx;
		iter->y = iter->nexty;
		glyph = fons__getGlyph(stash, iter->font, iter->codepoint, iter->isize, iter->iblur, iter->bitmapOption);
		// If the iterator was initialized with FONS_GLYPH_BITMAP_OPTIONAL, then the UV coordinates of the quad will be invalid.
		if (glyph != NULL)
			fons__getQuad(stash, iter->font, iter->prevGlyphIndex, glyph, iter->scale, iter->spacing, &iter->nextx, &iter->nexty, quad);
		iter->prevGlyphIndex = glyph != NULL ? glyph->index : -1;
		break;
	}
	iter->next = str;

	return 1;
}

#if FONS_RENDERING
void fonsDrawDebug(FONScontext* stash, float x, float y)
{
	int i;
	int w = stash->params.width;
	int h = stash->params.height;
	float u = w == 0 ? 0 : (1.0f / w);
	float v = h == 0 ? 0 : (1.0f / h);

	if (stash->nverts+6+6 > FONS_VERTEX_COUNT)
		fons__flush(stash);

	// Draw background
	fons__vertex(stash, x+0, y+0, u, v, 0x0fffffff);
	fons__vertex(stash, x+w, y+h, u, v, 0x0fffffff);
	fons__vertex(stash, x+w, y+0, u, v, 0x0fffffff);

	fons__vertex(stash, x+0, y+0, u, v, 0x0fffffff);
	fons__vertex(stash, x+0, y+h, u, v, 0x0fffffff);
	fons__vertex(stash, x+w, y+h, u, v, 0x0fffffff);

	// Draw texture
	fons__vertex(stash, x+0, y+0, 0, 0, 0xffffffff);
	fons__vertex(stash, x+w, y+h, 1, 1, 0xffffffff);
	fons__vertex(stash, x+w, y+0, 1, 0, 0xffffffff);

	fons__vertex(stash, x+0, y+0, 0, 0, 0xffffffff);
	fons__vertex(stash, x+0, y+h, 0, 1, 0xffffffff);
	fons__vertex(stash, x+w, y+h, 1, 1, 0xffffffff);

	// Drawbug draw atlas
	for (i = 0; i < stash->atlas->nnodes; i++) {
		FONSatlasNode* n = &stash->atlas->nodes[i];

		if (stash->nverts+6 > FONS_VERTEX_COUNT)
			fons__flush(stash);

		fons__vertex(stash, x+n->x+0, y+n->y+0, u, v, 0xc00000ff);
		fons__vertex(stash, x+n->x+n->width, y+n->y+1, u, v, 0xc00000ff);
		fons__vertex(stash, x+n->x+n->width, y+n->y+0, u, v, 0xc00000ff);

		fons__vertex(stash, x+n->x+0, y+n->y+0, u, v, 0xc00000ff);
		fons__vertex(stash, x+n->x+0, y+n->y+1, u, v, 0xc00000ff);
		fons__vertex(stash, x+n->x+n->width, y+n->y+1, u, v, 0xc00000ff);
	}

	fons__flush(stash);
}
#endif

float fonsTextBounds(FONScontext* stash,
					 float x, float y,
					 const char* str, const char* end,
					 float* bounds)
{
	if (stash == NULL) return 0;
	FONSstate* state = fons__getState(stash);
	unsigned int codepoint;
	unsigned int utf8state = 0;
	FONSquad q;
	FONSglyph* glyph = NULL;
	int prevGlyphIndex = -1;
	short isize = (short)(state->size*10.0f);
	short iblur = (short)state->blur;
	float scale;
	FONSfont* font;
	float startx, advance;
	float minx, miny, maxx, maxy;

	if (state->font < 0 || state->font >= stash->nfonts) return 0;
	font = stash->fonts[state->font];
	if (font->data == NULL) return 0;

	scale = fons__tt_getPixelHeightScale(&font->font, (float)isize/10.0f);

	// Align vertically.
	y += fons__getVertAlign(stash, font, state->align, isize);

	minx = maxx = x;
	miny = maxy = y;
	startx = x;

	if (end == NULL)
		end = str + strlen(str);

	for (; str != end; ++str) {
		if (fons__decutf8(&utf8state, &codepoint, *(const unsigned char*)str))
			continue;
		glyph = fons__getGlyph(stash, font, codepoint, isize, iblur, FONS_GLYPH_BITMAP_OPTIONAL);
		if (glyph != NULL) {
			fons__getQuad(stash, font, prevGlyphIndex, glyph, scale, state->spacing, &x, &y, &q);
			if (q.x0 < minx) minx = q.x0;
			if (q.x1 > maxx) maxx = q.x1;
			if (stash->params.flags & FONS_ZERO_TOPLEFT) {
				if (q.y0 < miny) miny = q.y0;
				if (q.y1 > maxy) maxy = q.y1;
			} else {
				if (q.y1 < miny) miny = q.y1;
				if (q.y0 > maxy) maxy = q.y0;
			}
		}
		prevGlyphIndex = glyph != NULL ? glyph->index : -1;
	}

	advance = x - startx;

	// Align horizontally
	if (state->align & FONS_ALIGN_LEFT) {
		// empty
	} else if (state->align & FONS_ALIGN_RIGHT) {
		minx -= advance;
		maxx -= advance;
	} else if (state->align & FONS_ALIGN_CENTER) {
		minx -= advance * 0.5f;
		maxx -= advance * 0.5f;
	}

	if (bounds) {
		bounds[0] = minx;
		bounds[1] = miny;
		bounds[2] = maxx;
		bounds[3] = maxy;
	}

	return advance;
}

void fonsVertMetrics(FONScontext* stash,
					 float* ascender, float* descender, float* lineh)
{
	if (stash == NULL) return;
	FONSfont* font;
	FONSstate* state = fons__getState(stash);
	short isize;

	if (state->font < 0 || state->font >= stash->nfonts) return;
	font = stash->fonts[state->font];
	isize = (short)(state->size*10.0f);
	if (font->data == NULL) return;

	if (ascender)
		*ascender = font->ascender*isize/10.0f;
	if (descender)
		*descender = font->descender*isize/10.0f;
	if (lineh)
		*lineh = font->lineh*isize/10.0f;
}

void fonsLineBounds(FONScontext* stash, float y, float* miny, float* maxy)
{
	if (stash == NULL) return;
	FONSfont* font;
	FONSstate* state = fons__getState(stash);
	short isize;

	if (state->font < 0 || state->font >= stash->nfonts) return;
	font = stash->fonts[state->font];
	isize = (short)(state->size*10.0f);
	if (font->data == NULL) return;

	y += fons__getVertAlign(stash, font, state->align, isize);

	if (stash->params.flags & FONS_ZERO_TOPLEFT) {
		*miny = y - font->ascender * (float)isize/10.0f;
		*maxy = *miny + font->lineh*isize/10.0f;
	} else {
		*maxy = y + font->descender * (float)isize/10.0f;
		*miny = *maxy - font->lineh*isize/10.0f;
	}
}

const unsigned char* fonsGetTextureData(FONScontext* stash, int* width, int* height)
{
	if (width != NULL)
		*width = stash->params.width;
	if (height != NULL)
		*height = stash->params.height;
	return stash->texData;
}

int fonsValidateTexture(FONScontext* stash, int* dirty)
{
	if (stash->dirtyRect[0] < stash->dirtyRect[2] && stash->dirtyRect[1] < stash->dirtyRect[3]) {
		dirty[0] = stash->dirtyRect[0];
		dirty[1] = stash->dirtyRect[1];
		dirty[2] = stash->dirtyRect[2];
		dirty[3] = stash->dirtyRect[3];
		// Reset dirty rect
		stash->dirtyRect[0] = stash->params.width;
		stash->dirtyRect[1] = stash->params.height;
		stash->dirtyRect[2] = 0;
		stash->dirtyRect[3] = 0;
		return 1;
	}
	return 0;
}

void fonsDeleteInternal(FONScontext* stash)
{
	int i;
	if (stash == NULL) return;

	if (stash->params.renderDelete)
		stash->params.renderDelete(stash->params.userPtr);

	for (i = 0; i < stash->nfonts; ++i)
		fons__freeFont(stash->fonts[i]);

	if (stash->atlas) fons__deleteAtlas(stash->atlas);
	if (stash->fonts) FONSfree(stash->fonts);
	if (stash->texData) FONSfree(stash->texData);
	if (stash->scratch) FONSfree(stash->scratch);
	FONSfree(stash);
}

void fonsSetErrorCallback(FONScontext* stash, void (*callback)(void* uptr, int error, int val), void* uptr)
{
	if (stash == NULL) return;
	stash->handleError = callback;
	stash->errorUptr = uptr;
}

void fonsGetAtlasSize(FONScontext* stash, int* width, int* height)
{
	if (stash == NULL) return;
	*width = stash->params.width;
	*height = stash->params.height;
}

int fonsExpandAtlas(FONScontext* stash, int width, int height)
{
	int i, maxy = 0;
	unsigned char* data = NULL;
	if (stash == NULL) return 0;

	width = fons__maxi(width, stash->params.width);
	height = fons__maxi(height, stash->params.height);

	if (width == stash->params.width && height == stash->params.height)
		return 1;

#if FONS_RENDERING
	// Flush pending glyphs.
	fons__flush(stash);
#endif

	// Create new texture
	if (stash->params.renderResize != NULL) {
		if (stash->params.renderResize(stash->params.userPtr, width, height) == 0)
			return 0;
	}
	// Copy old texture data over.
	data = (unsigned char*)FONSmalloc(width * height);
	if (data == NULL)
		return 0;
	for (i = 0; i < stash->params.height; i++) {
		unsigned char* dst = &data[i*width];
		unsigned char* src = &stash->texData[i*stash->params.width];
		memcpy(dst, src, stash->params.width);
		if (width > stash->params.width)
			memset(dst+stash->params.width, 0, width - stash->params.width);
	}
	if (height > stash->params.height)
		memset(&data[stash->params.height * width], 0, (height - stash->params.height) * width);

	FONSfree(stash->texData);
	stash->texData = data;

	// Increase atlas size
	fons__atlasExpand(stash->atlas, width, height);

	// Add existing data as dirty.
	for (i = 0; i < stash->atlas->nnodes; i++)
		maxy = fons__maxi(maxy, stash->atlas->nodes[i].y);
	stash->dirtyRect[0] = 0;
	stash->dirtyRect[1] = 0;
	stash->dirtyRect[2] = stash->params.width;
	stash->dirtyRect[3] = maxy;

	stash->params.width = width;
	stash->params.height = height;
	stash->itw = 1.0f/stash->params.width;
	stash->ith = 1.0f/stash->params.height;

	return 1;
}

int fonsResetAtlas(FONScontext* stash, int width, int height)
{
	int i, j;
	if (stash == NULL) return 0;

#if FONS_RENDERING
	// Flush pending glyphs.
	fons__flush(stash);
#endif

	// Create new texture
	if (stash->params.renderResize != NULL) {
		if (stash->params.renderResize(stash->params.userPtr, width, height) == 0)
			return 0;
	}

	// Reset atlas
	fons__atlasReset(stash->atlas, width, height);

	// Clear texture data.
	stash->texData = (unsigned char*)FONSrealloc(stash->texData, width * height);
	if (stash->texData == NULL) return 0;
	memset(stash->texData, 0, width * height);

	// Reset dirty rect
	stash->dirtyRect[0] = width;
	stash->dirtyRect[1] = height;
	stash->dirtyRect[2] = 0;
	stash->dirtyRect[3] = 0;

	// Reset cached glyphs
	for (i = 0; i < stash->nfonts; i++) {
		FONSfont* font = stash->fonts[i];
		font->nglyphs = 0;
		for (j = 0; j < FONS_HASH_LUT_SIZE; j++) {
			font->lut[j] = -1;
		}
	}

	stash->params.width = width;
	stash->params.height = height;
	stash->itw = 1.0f/stash->params.width;
	stash->ith = 1.0f/stash->params.height;

	// Add white rect at 0,0 for debug drawing.
#if FONS_CUSTOM_WHITE_RECT
	const int wrw = stash->params.whiteRectWidth <= 0 ? 2 : stash->params.whiteRectWidth;
	const int wrh = stash->params.whiteRectHeight <= 0 ? 2 : stash->params.whiteRectHeight;
	fons__addWhiteRect(stash, wrw, wrh);
#else
	fons__addWhiteRect(stash, 2,2);
#endif

	stash->atlasID++;

	return 1;
}

void fonsInitString(FONSstring* str)
{
	memset(str, 0, sizeof(FONSstring));
}

void fonsDestroyString(FONSstring* str)
{
	FONSfree(str->m_Quads);
	str->m_Quads = 0;
	FONSfree(str->m_Codepoints);
	str->m_Codepoints = 0;
	FONSfree(str->m_GlyphIndices);
	str->m_GlyphIndices = 0;
	FONSfree(str->m_KernAdv);
	str->m_KernAdv = 0;
	str->m_Length = 0;
	str->m_Capacity = 0;
}

void fonsResetString(FONScontext* stash, FONSstring* str, const char* text, const char* end)
{
	// Worst case scenario: 1 codepoint per char.
	const unsigned int len = (unsigned int)(end ? (end - text) : strlen(text));
	if(len > str->m_Capacity) {
		str->m_Capacity = len;

		str->m_Quads = (FONSquad*)FONSrealloc(str->m_Quads, sizeof(FONSquad) * len);
		str->m_Codepoints = (unsigned int*)FONSrealloc(str->m_Codepoints, sizeof(unsigned int) * len);
		str->m_GlyphIndices = (int*)FONSrealloc(str->m_GlyphIndices, sizeof(int) * len);
		str->m_KernAdv = (int*)FONSrealloc(str->m_KernAdv, sizeof(int) * len);
	}

	FONSstate* state = fons__getState(stash);
	FONSfont* font = stash->fonts[state->font];

	int prevGlyphIndex = -1;

	unsigned int utf8state = 0;
	unsigned int* cp = str->m_Codepoints;
	int* gi = str->m_GlyphIndices;
	int* ka = str->m_KernAdv;
	for (unsigned int i = 0;i < len;++i) {
		if (fons__decutf8(&utf8state, cp, (unsigned char)text[i])) {
			continue;
		}

		*gi = fons__tt_getGlyphIndex(&font->font, *cp);
		if(prevGlyphIndex == -1) {
			*ka = 0;
		} else {
			*ka = fons__tt_getGlyphKernAdvance(&font->font, prevGlyphIndex, *gi);
		}

		prevGlyphIndex = *gi;
		++cp;
		++gi;
		++ka;
	}

	str->m_Length = (unsigned int)(cp - str->m_Codepoints);

	str->m_LastBakeAtlasID = 0;
}

static FONSglyph* fons__bakeGlyph(FONScontext* stash, FONSfont* font, int ascii_to_glyph_index, unsigned int codepoint, short isize, short iblur)
{
	FONSglyph* glyph = NULL;
	FONSfont* renderFont = font;
	const float size = (float)isize / 10.0f;
	const int pad = iblur + 2;

	// Find code point and size.
#if FONS_SEPARATE_CODEPOINT
	const unsigned int h = fons__hashint(codepoint) & (FONS_HASH_LUT_SIZE-1);
#else
	const uint64_t glyphCode = MAKE_GLYPH_CODE(codepoint, isize, iblur);
	const unsigned int h = fons__hashGlyphCode(glyphCode) & (FONS_HASH_LUT_SIZE - 1);
#endif

	// TODO: JD: This loop is the hottest part of the function under normal usage (i.e. all required glyphs are already cached).
	// The alternative hash function helps with glyph distribution in the LUT but we still have enough collisions to make the
	// loop execute multiple times per call. Larger LUT doesn't seem to help (in fact they seem to hurt perf).
	// It might help if the jumping around in memory from glyph to glyph was avoided (i.e. glyphs in the same bucket are
	// stored sequentially in the glyphs array) (No it doesn't!).
	int i = font->lut[h];
	while (i != -1) {
#if FONS_SEPARATE_CODEPOINT
		if (font->glyphs[i].codepoint == codepoint && font->glyphs[i].size == isize && font->glyphs[i].blur == iblur) {
			glyph = &font->glyphs[i];
			if (glyph->x0 >= 0 && glyph->y0 >= 0) {
				return glyph;
			}

			// At this point, glyph exists but the bitmap data is not yet created.
			break;
		}
#else
		if (font->glyphs[i].glyphCode == glyphCode) {
			glyph = &font->glyphs[i];
			if (glyph->x0 >= 0 && glyph->y0 >= 0) {
				return glyph;
			}

			// At this point, glyph exists but the bitmap data is not yet created.
			break;
		}
#endif
		i = font->glyphs[i].next;
	}

	// Could not find glyph, create it.
//	g = fons__tt_getGlyphIndex(&font->font, codepoint);
	// Try to find the glyph in fallback fonts.
	if (ascii_to_glyph_index == 0) {
		for (i = 0; i < font->nfallbacks; ++i) {
			FONSfont* fallbackFont = stash->fonts[font->fallbacks[i]];
			int fallbackIndex = fons__tt_getGlyphIndex(&fallbackFont->font, codepoint);
			if (fallbackIndex != 0) {
				ascii_to_glyph_index = fallbackIndex;
				renderFont = fallbackFont;
				break;
			}
		}
		// It is possible that we did not find a fallback glyph.
		// In that case the glyph index 'g' is 0, and we'll proceed below and cache empty glyph.
	}

	const float scale = fons__tt_getPixelHeightScale(&renderFont->font, size);

	int advance, lsb, x0, y0, x1, y1, x, y;
	fons__tt_buildGlyphBitmap(&renderFont->font, &font->font, ascii_to_glyph_index, size, scale, &advance, &lsb, &x0, &y0, &x1, &y1);

	int gw = x1 - x0 + pad * 2;
	int gh = y1 - y0 + pad * 2;

	// Find free spot for the rect in the atlas
	int gx, gy;
	int added = fons__atlasAddRect(stash->atlas, gw, gh, &gx, &gy);
	if (added == 0 && stash->handleError != NULL) {
		// Atlas is full, let the user to resize the atlas (or not), and try again.
		stash->handleError(stash->errorUptr, FONS_ATLAS_FULL, 0);
		added = fons__atlasAddRect(stash->atlas, gw, gh, &gx, &gy);
	}

	if (added == 0) {
		return NULL;
	}

	// Init glyph.
	if (glyph == NULL) {
		glyph = fons__allocGlyph(font);
#if FONS_SEPARATE_CODEPOINT
		glyph->codepoint = codepoint;
		glyph->size = isize;
		glyph->blur = iblur;
#else
		glyph->glyphCode = MAKE_GLYPH_CODE(codepoint, isize, iblur);
#endif

		// Insert char to hash lookup.
		glyph->next = font->lut[h];
		font->lut[h] = font->nglyphs - 1;
	}

	glyph->index = ascii_to_glyph_index;
	glyph->x0 = (short)gx;
	glyph->y0 = (short)gy;
	glyph->x1 = (short)(glyph->x0 + gw);
	glyph->y1 = (short)(glyph->y0 + gh);
	glyph->xadv = (short)(scale * advance * 10.0f);
	glyph->xoff = (short)(x0 - pad);
	glyph->yoff = (short)(y0 - pad);

	// Rasterize
	unsigned char* dst = &stash->texData[(glyph->x0 + pad) + (glyph->y0 + pad) * stash->params.width];
	fons__tt_renderGlyphBitmap(&renderFont->font, dst, gw - pad * 2, gh - pad * 2, stash->params.width, scale, scale, ascii_to_glyph_index);

	// Make sure there is one pixel empty border.
	dst = &stash->texData[glyph->x0 + glyph->y0 * stash->params.width];
	for (y = 0; y < gh; y++) {
		dst[y*stash->params.width] = 0;
		dst[gw - 1 + y*stash->params.width] = 0;
	}
	for (x = 0; x < gw; x++) {
		dst[x] = 0;
		dst[x + (gh - 1)*stash->params.width] = 0;
	}

	// Blur
	if (iblur > 0) {
		stash->nscratch = 0;

		unsigned char* bdst = &stash->texData[glyph->x0 + glyph->y0 * stash->params.width];
		fons__blur(stash, bdst, gw, gh, stash->params.width, iblur);
	}

	stash->dirtyRect[0] = fons__mini(stash->dirtyRect[0], glyph->x0);
	stash->dirtyRect[1] = fons__mini(stash->dirtyRect[1], glyph->y0);
	stash->dirtyRect[2] = fons__maxi(stash->dirtyRect[2], glyph->x1);
	stash->dirtyRect[3] = fons__maxi(stash->dirtyRect[3], glyph->y1);

	return glyph;
}

int fonsBakeString(FONScontext* stash, FONSstring* str)
{
	// Check if string is already baked on the current atlas.
	if(stash->atlasID == str->m_LastBakeAtlasID) {
		return (int)str->m_Length;
	}

	FONSstate* state = fons__getState(stash);
	const short isize = (short)(state->size * 10.0f);
	if(isize < 2) {
		return 0; // Font size too small. Don't render anything.
	}

	FONSfont* font = stash->fonts[state->font];
	const short iblur = (short)(state->blur > 20.0f ? 20 : state->blur);
	const float scale = fons__tt_getPixelHeightScale(&font->font, (float)isize / 10.0f);
	const float spacing = state->spacing;

	float x = 0.0f;
	float y = 0.0f;
	float minx = 0.0f, maxx = 0.0f;
	float miny = 0.0f, maxy = 0.0f;

	const unsigned int len = str->m_Length;
	for(unsigned int i = 0;i < len;++i) {
		const unsigned int cp = str->m_Codepoints[i];
		const int gi = str->m_GlyphIndices[i];
		const int kernAdv = str->m_KernAdv[i];

		FONSquad* q = &str->m_Quads[i];

		FONSglyph* glyph = fons__bakeGlyph(stash, font, gi, cp, isize, iblur);
		if(!glyph) {
			// Failed to insert glyph into atlas.
			return -1;
		}

		// fons__getQuad() inlined to avoid calling fons__tt_getGlyphKernAdvance
#if FONS_SNAP_TO_GRID
		x += (int)((kernAdv * scale) + spacing + 0.5f);
#else
		x += (kernAdv * scale) + spacing;
#endif

		// Each glyph has 2px border to allow good interpolation,
		// one pixel to prevent leaking, and one to allow good interpolation for rendering.
		// Inset the texture region by one pixel for correct interpolation.
		float xoff = (float)(glyph->xoff + 1);
		float yoff = (float)(glyph->yoff + 1);
		float x0 = (float)(glyph->x0 + 1);
		float y0 = (float)(glyph->y0 + 1);
		float x1 = (float)(glyph->x1 - 1);
		float y1 = (float)(glyph->y1 - 1);

		if (stash->params.flags & FONS_ZERO_TOPLEFT) {
#if FONS_SNAP_TO_GRID
			const float rx = (float)(int)(x + xoff);
			const float ry = (float)(int)(y + yoff);
#else
			const float rx = x + xoff;
			const float ry = y + yoff;
#endif

			q->x0 = rx;
			q->y0 = ry;
			q->x1 = rx + x1 - x0;
			q->y1 = ry + y1 - y0;

			q->s0 = x0 * stash->itw;
			q->t0 = y0 * stash->ith;
			q->s1 = x1 * stash->itw;
			q->t1 = y1 * stash->ith;
		} else {
#if FONS_SNAP_TO_GRID
			const float rx = (float)(int)(x + xoff);
			const float ry = (float)(int)(y - yoff);
#else
			const float rx = x + xoff;
			const float ry = y - yoff;
#endif

			q->x0 = rx;
			q->y0 = ry;
			q->x1 = rx + x1 - x0;
			q->y1 = ry - y1 + y0;

			q->s0 = x0 * stash->itw;
			q->t0 = y0 * stash->ith;
			q->s1 = x1 * stash->itw;
			q->t1 = y1 * stash->ith;
		}

#if FONS_SNAP_TO_GRID
		x += (int)(glyph->xadv / 10.0f + 0.5f);
#else
		x += glyph->xadv / 10.0f;
#endif

		if (q->x0 < minx) { minx = q->x0; }
		if (q->x1 > maxx) { maxx = q->x1; }
		if (stash->params.flags & FONS_ZERO_TOPLEFT) {
			if (q->y0 < miny) { miny = q->y0; }
			if (q->y1 > maxy) { maxy = q->y1; }
		} else {
			if (q->y1 < miny) { miny = q->y1; }
			if (q->y0 > maxy) { maxy = q->y0; }
		}
	}

	str->m_Width = x;
	str->m_Bounds[0] = minx;
	str->m_Bounds[1] = miny;
	str->m_Bounds[2] = maxx;
	str->m_Bounds[3] = maxy;

	str->m_LastBakeAtlasID = stash->atlasID;

	return (int)str->m_Length;
}

float fonsAlignString(FONScontext* stash, FONSstring* str, unsigned int align, float* x, float* y)
{
	FONSstate* state = fons__getState(stash);
	FONSfont* font = stash->fonts[state->font];
	const short isize = (short)(state->size * 10.0f);

	const float width = str->m_Width;

	if(x) {
		if (align & FONS_ALIGN_LEFT) {
			// empty
		} else if (align & FONS_ALIGN_RIGHT) {
			*x -= width;
		} else if (align & FONS_ALIGN_CENTER) {
			*x -= width * 0.5f;
		}
	}

	if(y) {
		*y += fons__getVertAlign(stash, font, align, isize);
	}

	return width;
}

#endif
