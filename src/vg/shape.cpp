#include "shape.h"
#include <string.h> // strlen

namespace vg
{
inline void* allocCommand(bx::MemoryBlockI* memBlock, uint32_t cmdSize)
{
	uint32_t curMemBlockSize = memBlock->getSize();
	void* buffer = memBlock->more(cmdSize);
	return (uint8_t*)buffer + curMemBlockSize;
}

void Shape::BeginPath()
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::BeginPath);
}

void Shape::MoveTo(float x, float y)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 2;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::MoveTo);
	bx::write(&writer, x);
	bx::write(&writer, y);
}

void Shape::LineTo(float x, float y)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 2;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::LineTo);
	bx::write(&writer, x);
	bx::write(&writer, y);
}

void Shape::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 6;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::BezierTo);
	bx::write(&writer, c1x);
	bx::write(&writer, c1y);
	bx::write(&writer, c2x);
	bx::write(&writer, c2y);
	bx::write(&writer, x);
	bx::write(&writer, y);
}

void Shape::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 5;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::ArcTo);
	bx::write(&writer, x1);
	bx::write(&writer, y1);
	bx::write(&writer, x2);
	bx::write(&writer, y2);
	bx::write(&writer, radius);
}

void Shape::Rect(float x, float y, float w, float h)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 4;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::Rect);
	bx::write(&writer, x);
	bx::write(&writer, y);
	bx::write(&writer, w);
	bx::write(&writer, h);
}

void Shape::RoundedRect(float x, float y, float w, float h, float r)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 5;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::RoundedRect);
	bx::write(&writer, x);
	bx::write(&writer, y);
	bx::write(&writer, w);
	bx::write(&writer, h);
	bx::write(&writer, r);
}

void Shape::Circle(float cx, float cy, float radius)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 3;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::Circle);
	bx::write(&writer, cx);
	bx::write(&writer, cy);
	bx::write(&writer, radius);
}

void Shape::ClosePath()
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::ClosePath);
}

void Shape::FillConvexPath(Color col, bool aa)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(Color) + sizeof(bool);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::FillConvexColor);
	bx::write(&writer, col);
	bx::write(&writer, aa);
}

void Shape::FillConvexPath(GradientHandle gradient, bool aa)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(GradientHandle) + sizeof(bool);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::FillConvexGradient);
	bx::write(&writer, gradient);
	bx::write(&writer, aa);
}

void Shape::FillConvexPath(ImagePatternHandle img, bool aa)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(ImagePatternHandle) + sizeof(bool);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::FillConvexImage);
	bx::write(&writer, img);
	bx::write(&writer, aa);
}

void Shape::FillConcavePath(Color col, bool aa)
{
	// NOTE: Concave polygon decomposition cannot be done at this point because it requires all the 
	// previously cached commands to be executed (which in turn requires a rendering context).
	// It should be the responsibility of the renderer to cache the final shape.
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(Color) + sizeof(bool);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::FillConcaveColor);
	bx::write(&writer, col);
	bx::write(&writer, aa);
}

void Shape::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(Color) + sizeof(float) + sizeof(bool) + sizeof(LineCap::Enum) + sizeof(LineJoin::Enum);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::Stroke);
	bx::write(&writer, col);
	bx::write(&writer, width);
	bx::write(&writer, aa);
	bx::write(&writer, lineCap);
	bx::write(&writer, lineJoin);
}

GradientHandle Shape::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 4 + sizeof(Color) * 2;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::LinearGradient);
	bx::write(&writer, sx);
	bx::write(&writer, sy);
	bx::write(&writer, ex);
	bx::write(&writer, ey);
	bx::write(&writer, icol);
	bx::write(&writer, ocol);

	return { gradientHandle };
}

GradientHandle Shape::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 6 + sizeof(Color) * 2;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::BoxGradient);
	bx::write(&writer, x);
	bx::write(&writer, y);
	bx::write(&writer, w);
	bx::write(&writer, h);
	bx::write(&writer, r);
	bx::write(&writer, f);
	bx::write(&writer, icol);
	bx::write(&writer, ocol);

	return { gradientHandle };
}

GradientHandle Shape::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 4 + sizeof(Color) * 2;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::RadialGradient);
	bx::write(&writer, cx);
	bx::write(&writer, cy);
	bx::write(&writer, inr);
	bx::write(&writer, outr);
	bx::write(&writer, icol);
	bx::write(&writer, ocol);

	return { gradientHandle };
}

ImagePatternHandle Shape::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(float) * 6 + sizeof(ImageHandle);
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::ImagePattern);
	bx::write(&writer, cx);
	bx::write(&writer, cy);
	bx::write(&writer, w);
	bx::write(&writer, h);
	bx::write(&writer, angle);
	bx::write(&writer, image);
	bx::write(&writer, alpha);

	return { gradientHandle };
}

void Shape::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	uint32_t len = (uint32_t)(end != nullptr ? end - text : strlen(text));

	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(Font) + sizeof(uint32_t) * 2 + sizeof(Color) + sizeof(float) * 2 + len;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::TextStatic);
	bx::write(&writer, font);
	bx::write(&writer, alignment);
	bx::write(&writer, color);
	bx::write(&writer, x);
	bx::write(&writer, y);
	bx::write(&writer, len);
	bx::write(&writer, text, len);

	m_Flags |= ShapeFlag::HasText;
}

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
void Shape::TextDynamic(const Font& font, uint32_t alignment, Color color, float x, float y, uint32_t stringID)
{
	const size_t cmdSize = sizeof(ShapeCommand::Enum) + sizeof(Font) + sizeof(uint32_t) * 2 + sizeof(Color) + sizeof(float) * 2;
	void* cmd = allocCommand(m_CmdList, (uint32_t)cmdSize);

	bx::StaticMemoryBlockWriter writer(cmd, (uint32_t)cmdSize);
	bx::write(&writer, ShapeCommand::TextDynamic);
	bx::write(&writer, font);
	bx::write(&writer, alignment);
	bx::write(&writer, color);
	bx::write(&writer, x);
	bx::write(&writer, y);
	bx::write(&writer, stringID);

	m_Flags |= ShapeFlag::HasText | ShapeFlag::HasDynamicText;
}
#endif
}
