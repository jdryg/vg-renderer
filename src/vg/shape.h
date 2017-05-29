#ifndef VG_SHAPE_H
#define VG_SHAPE_H

#include <stdint.h>
#include <bx/readerwriter.h>
#include "common.h"

namespace vg
{
struct ShapeCommand
{
	enum Enum : uint8_t // Max 256 commands should be enough
	{
		BeginPath = 0,
		ClosePath,
		MoveTo,
		LineTo,
		BezierTo,
		ArcTo,
		Rect,
		RoundedRect,
		Circle,

		FillConvexColor,
		FillConvexGradient,
		FillConvexImage,
		FillConcaveColor,
		Stroke,

		LinearGradient,
		BoxGradient,
		RadialGradient,
		ImagePattern,

		TextStatic, 
#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
		TextDynamic,
#endif
	};
};

struct ShapeFlag
{
	enum Enum : uint32_t
	{
		HasText = 0x00000001,
		HasDynamicText = 0x00000002,
		HasGradients = 0x00000004,
		HasImages = 0x00000008,
		AllowCommandReordering = 0x00000010,
		EnableCaching = 0x00000020
	};
};

struct Shape
{
	bx::MemoryBlockI* m_CmdList;
	void* m_RendererData;
	uint16_t m_NumGradients;
	uint16_t m_NumImagePatterns;
	uint32_t m_Flags;

	// NOTE: The shape doesn't own the memory block.
	Shape(bx::MemoryBlockI* memBlock) :
		m_CmdList(memBlock),
		m_NumGradients(0),
		m_NumImagePatterns(0),
		m_Flags(0),
		m_RendererData(nullptr)
	{}

	~Shape()
	{}

	void BeginPath();
	void MoveTo(float x, float y);
	void LineTo(float x, float y);
	void BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
	void ArcTo(float x1, float y1, float x2, float y2, float radius);
	void Rect(float x, float y, float w, float h);
	void RoundedRect(float x, float y, float w, float h, float r);
	void Circle(float cx, float cy, float radius);
	void ClosePath();
	void FillConvexPath(Color col, bool aa);
	void FillConvexPath(GradientHandle gradient, bool aa);
	void FillConvexPath(ImagePatternHandle img, bool aa);
	void FillConcavePath(Color col, bool aa);
	void StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap = LineCap::Butt, LineJoin::Enum lineJoin = LineJoin::Miter);

	GradientHandle LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol);
	GradientHandle BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol);
	GradientHandle RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol);
	ImagePatternHandle ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha);

	void Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end);

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
	void TextDynamic(const Font& font, uint32_t alignment, Color color, float x, float y, uint32_t stringID);
#endif
};
}

#endif
