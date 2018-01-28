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
		m_NumGradients = 0;
		m_NumImagePatterns = 0;
	}
}

void Shape::BeginPath()
{
	bx::write(&m_CmdListWriter, ShapeCommand::BeginPath);
}

void Shape::MoveTo(float x, float y)
{
	const float coords[2] = { x, y };
	bx::write(&m_CmdListWriter, ShapeCommand::MoveTo);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 2);
}

void Shape::LineTo(float x, float y)
{
	const float coords[2] = { x, y };
	bx::write(&m_CmdListWriter, ShapeCommand::LineTo);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 2);
}

void Shape::BezierTo(float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	const float coords[6] = { c1x, c1y, c2x, c2y, x, y };
	bx::write(&m_CmdListWriter, ShapeCommand::BezierTo);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 6);
}

void Shape::ArcTo(float x1, float y1, float x2, float y2, float radius)
{
	const float coords[5] = { x1, y1, x2, y2, radius };
	bx::write(&m_CmdListWriter, ShapeCommand::ArcTo);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 5);
}

void Shape::Rect(float x, float y, float w, float h)
{
	const float coords[4] = { x, y, w, h };
	bx::write(&m_CmdListWriter, ShapeCommand::Rect);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 4);
}

void Shape::RoundedRect(float x, float y, float w, float h, float r)
{
	const float coords[5] = { x, y, w, h, r };
	bx::write(&m_CmdListWriter, ShapeCommand::RoundedRect);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 5);
}

void Shape::RoundedRectVarying(float x, float y, float w, float h, float rtl, float rbl, float rbr, float rtr)
{
	const float coords[8] = { x, y, w, h, rtl, rbl, rbr, rtr };
	bx::write(&m_CmdListWriter, ShapeCommand::RoundedRectVarying);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 8);
}

void Shape::Circle(float cx, float cy, float radius)
{
	const float coords[3] = { cx, cy, radius };
	bx::write(&m_CmdListWriter, ShapeCommand::Circle);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 3);
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

	const float grad[] = { sx, sy, ex, ey };
	const Color col[] = { icol, ocol };
	bx::write(&m_CmdListWriter, ShapeCommand::LinearGradient);
	bx::write(&m_CmdListWriter, &grad[0], sizeof(float) * 4);
	bx::write(&m_CmdListWriter, &col[0], sizeof(Color) * 2);

	return { gradientHandle };
}

GradientHandle Shape::BoxGradient(float x, float y, float w, float h, float r, float f, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const float grad[] = { x, y, w, h, r, f };
	const Color col[] = { icol, ocol };
	bx::write(&m_CmdListWriter, ShapeCommand::BoxGradient);
	bx::write(&m_CmdListWriter, &grad[0], sizeof(float) * 6);
	bx::write(&m_CmdListWriter, &col[0], sizeof(Color) * 2);

	return { gradientHandle };
}

GradientHandle Shape::RadialGradient(float cx, float cy, float inr, float outr, Color icol, Color ocol)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const float grad[] = { cx, cy, inr, outr };
	const Color col[] = { icol, ocol };
	bx::write(&m_CmdListWriter, ShapeCommand::RadialGradient);
	bx::write(&m_CmdListWriter, &grad[0], sizeof(float) * 4);
	bx::write(&m_CmdListWriter, &col[0], sizeof(float) * 2);

	return { gradientHandle };
}

ImagePatternHandle Shape::ImagePattern(float cx, float cy, float w, float h, float angle, ImageHandle image, float alpha)
{
	const uint16_t gradientHandle = m_NumGradients++;

	const float params[] = { cx, cy, w, h, angle, alpha };
	bx::write(&m_CmdListWriter, ShapeCommand::ImagePattern);
	bx::write(&m_CmdListWriter, &params[0], sizeof(float) * 6);
	bx::write(&m_CmdListWriter, image);

	return { gradientHandle };
}

void Shape::Text(const Font& font, uint32_t alignment, Color color, float x, float y, const char* text, const char* end)
{
	uint32_t len = (uint32_t)(end != nullptr ? end - text : bx::strLen(text));

	ShapeCommandText cmd;
	cmd.font = font;
	cmd.alignment = alignment;
	cmd.col = color;
	cmd.x = x;
	cmd.y = y;
	cmd.len = len;

	bx::write(&m_CmdListWriter, ShapeCommand::TextStatic);
	bx::write(&m_CmdListWriter, cmd);
	bx::write(&m_CmdListWriter, text, len);

	m_Flags |= ShapeFlag::HasText;
}

#if VG_CONFIG_SHAPE_DYNAMIC_TEXT
void Shape::TextDynamic(const Font& font, uint32_t alignment, Color color, float x, float y, uint32_t stringID)
{
	ShapeCommandText cmd;
	cmd.font = font;
	cmd.alignment = alignment;
	cmd.col = color;
	cmd.x = x;
	cmd.y = y;
	cmd.len = stringID;

	bx::write(&m_CmdListWriter, ShapeCommand::TextDynamic);
	bx::write(&m_CmdListWriter, cmd);

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
	const float coords[] = { x, y, w, h };
	bx::write(&m_CmdListWriter, ShapeCommand::Scissor);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 4);
}

void Shape::IntersectScissor(float x, float y, float w, float h)
{
	const float coords[] = { x, y, w, h };
	bx::write(&m_CmdListWriter, ShapeCommand::IntersectScissor);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 4);
}

void Shape::Rotate(float ang_rad)
{
	bx::write(&m_CmdListWriter, ShapeCommand::Rotate);
	bx::write(&m_CmdListWriter, ang_rad);
}

void Shape::Translate(float x, float y)
{
	const float coords[] = { x, y };
	bx::write(&m_CmdListWriter, ShapeCommand::Translate);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 2);
}

void Shape::Scale(float x, float y)
{
	const float coords[] = { x, y };
	bx::write(&m_CmdListWriter, ShapeCommand::Scale);
	bx::write(&m_CmdListWriter, &coords[0], sizeof(float) * 2);
}

void Shape::BeginClip(ClipRule::Enum rule)
{
	bx::write(&m_CmdListWriter, ShapeCommand::BeginClip);
	bx::write(&m_CmdListWriter, rule);
}

void Shape::EndClip()
{
	bx::write(&m_CmdListWriter, ShapeCommand::EndClip);
}

void Shape::ResetClip()
{
	bx::write(&m_CmdListWriter, ShapeCommand::ResetClip);
}
}
