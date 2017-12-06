#include "shape.h"
#include <bx/string.h>

namespace vg
{
BX_PRAGMA_DIAGNOSTIC_IGNORED_MSVC(4127) // conditional expression is constant

void Shape::Reset()
{
	VG_CHECK(!m_RendererData, "Cannot reset shape if it's already cached. Disable caching for this shape to be able to reset it.");
	if (!m_RendererData) {
		m_CmdListWriter.seek(0, bx::Whence::Begin);
	}
}

void Shape::BeginPath()
{
	bx::write(&m_CmdListWriter, ShapeCommand::BeginPath);
}

void Shape::MoveTo(float x, float y)
{
	bx::write(&m_CmdListWriter, ShapeCommand::MoveTo);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
}

void Shape::LineTo(float x, float y)
{
	bx::write(&m_CmdListWriter, ShapeCommand::LineTo);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
}

void Shape::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	bx::write(&m_CmdListWriter, ShapeCommand::BezierTo);
	bx::write(&m_CmdListWriter, c1x);
	bx::write(&m_CmdListWriter, c1y);
	bx::write(&m_CmdListWriter, c2x);
	bx::write(&m_CmdListWriter, c2y);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
}

void Shape::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	bx::write(&m_CmdListWriter, ShapeCommand::ArcTo);
	bx::write(&m_CmdListWriter, x1);
	bx::write(&m_CmdListWriter, y1);
	bx::write(&m_CmdListWriter, x2);
	bx::write(&m_CmdListWriter, y2);
	bx::write(&m_CmdListWriter, radius);
}

void Shape::Rect(float x, float y, float w, float h)
{
	bx::write(&m_CmdListWriter, ShapeCommand::Rect);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
	bx::write(&m_CmdListWriter, w);
	bx::write(&m_CmdListWriter, h);
}

void Shape::RoundedRect(float x, float y, float w, float h, float r)
{
	bx::write(&m_CmdListWriter, ShapeCommand::RoundedRect);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
	bx::write(&m_CmdListWriter, w);
	bx::write(&m_CmdListWriter, h);
	bx::write(&m_CmdListWriter, r);
}

void Shape::Circle(float cx, float cy, float radius)
{
	bx::write(&m_CmdListWriter, ShapeCommand::Circle);
	bx::write(&m_CmdListWriter, cx);
	bx::write(&m_CmdListWriter, cy);
	bx::write(&m_CmdListWriter, radius);
}

void Shape::ClosePath()
{
	bx::write(&m_CmdListWriter, ShapeCommand::ClosePath);
}

void Shape::FillConvexPath(Color col, bool aa)
{
	bx::write(&m_CmdListWriter, ShapeCommand::FillConvexColor);
	bx::write(&m_CmdListWriter, col);
	bx::write(&m_CmdListWriter, aa);
}

void Shape::FillConvexPath(GradientHandle gradient, bool aa)
{
	bx::write(&m_CmdListWriter, ShapeCommand::FillConvexGradient);
	bx::write(&m_CmdListWriter, gradient);
	bx::write(&m_CmdListWriter, aa);
}

void Shape::FillConvexPath(ImagePatternHandle img, bool aa)
{
	bx::write(&m_CmdListWriter, ShapeCommand::FillConvexImage);
	bx::write(&m_CmdListWriter, img);
	bx::write(&m_CmdListWriter, aa);
}

void Shape::FillConcavePath(Color col, bool aa)
{
	// NOTE: Concave polygon decomposition cannot be done at this point because it requires all the 
	// previously cached commands to be executed (which in turn requires a rendering context).
	// It should be the responsibility of the renderer to cache the final shape.
	bx::write(&m_CmdListWriter, ShapeCommand::FillConcaveColor);
	bx::write(&m_CmdListWriter, col);
	bx::write(&m_CmdListWriter, aa);
}

void Shape::StrokePath(Color col, float width, bool aa, LineCap::Enum lineCap, LineJoin::Enum lineJoin)
{
	bx::write(&m_CmdListWriter, ShapeCommand::Stroke);
	bx::write(&m_CmdListWriter, col);
	bx::write(&m_CmdListWriter, width);
	bx::write(&m_CmdListWriter, aa);
	bx::write(&m_CmdListWriter, lineCap);
	bx::write(&m_CmdListWriter, lineJoin);
}

GradientHandle Shape::LinearGradient(float sx, float sy, float ex, float ey, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	bx::write(&m_CmdListWriter, ShapeCommand::LinearGradient);
	bx::write(&m_CmdListWriter, sx);
	bx::write(&m_CmdListWriter, sy);
	bx::write(&m_CmdListWriter, ex);
	bx::write(&m_CmdListWriter, ey);
	bx::write(&m_CmdListWriter, icol);
	bx::write(&m_CmdListWriter, ocol);

	return { gradientHandle };
}

GradientHandle Shape::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	bx::write(&m_CmdListWriter, ShapeCommand::BoxGradient);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
	bx::write(&m_CmdListWriter, w);
	bx::write(&m_CmdListWriter, h);
	bx::write(&m_CmdListWriter, r);
	bx::write(&m_CmdListWriter, f);
	bx::write(&m_CmdListWriter, icol);
	bx::write(&m_CmdListWriter, ocol);

	return { gradientHandle };
}

GradientHandle Shape::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	bx::write(&m_CmdListWriter, ShapeCommand::RadialGradient);
	bx::write(&m_CmdListWriter, cx);
	bx::write(&m_CmdListWriter, cy);
	bx::write(&m_CmdListWriter, inr);
	bx::write(&m_CmdListWriter, outr);
	bx::write(&m_CmdListWriter, icol);
	bx::write(&m_CmdListWriter, ocol);

	return { gradientHandle };
}

ImagePatternHandle Shape::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	const uint16_t gradientHandle = m_NumGradients++;

	bx::write(&m_CmdListWriter, ShapeCommand::ImagePattern);
	bx::write(&m_CmdListWriter, cx);
	bx::write(&m_CmdListWriter, cy);
	bx::write(&m_CmdListWriter, w);
	bx::write(&m_CmdListWriter, h);
	bx::write(&m_CmdListWriter, angle);
	bx::write(&m_CmdListWriter, image);
	bx::write(&m_CmdListWriter, alpha);

	return { gradientHandle };
}

void Shape::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	uint32_t len = (uint32_t)(end != nullptr ? end - text : bx::strLen(text));

	bx::write(&m_CmdListWriter, ShapeCommand::TextStatic);
	bx::write(&m_CmdListWriter, font);
	bx::write(&m_CmdListWriter, alignment);
	bx::write(&m_CmdListWriter, color);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
	bx::write(&m_CmdListWriter, len);
	bx::write(&m_CmdListWriter, text, len);

	m_Flags |= ShapeFlag::HasText;
}

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
void Shape::TextDynamic(const Font& font, uint32_t alignment, Color color, float x, float y, uint32_t stringID)
{
	bx::write(&m_CmdListWriter, ShapeCommand::TextDynamic);
	bx::write(&m_CmdListWriter, font);
	bx::write(&m_CmdListWriter, alignment);
	bx::write(&m_CmdListWriter, color);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
	bx::write(&m_CmdListWriter, stringID);

	m_Flags |= ShapeFlag::HasText | ShapeFlag::HasDynamicText;
}
#endif

void Shape::PushState()
{
	bx::write(&m_CmdListWriter, ShapeCommand::PushState);
}

void Shape::PopState()
{
	bx::write(&m_CmdListWriter, ShapeCommand::PopState);
}

void Shape::Scissor(float x, float y, float w, float h)
{
	bx::write(&m_CmdListWriter, ShapeCommand::Scissor);
	bx::write(&m_CmdListWriter, x);
	bx::write(&m_CmdListWriter, y);
	bx::write(&m_CmdListWriter, w);
	bx::write(&m_CmdListWriter, h);
}
}
