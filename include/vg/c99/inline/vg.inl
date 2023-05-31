#ifndef VG_VG_H
#error "Must be included from vg.h"
#endif

static inline float vg_clampf(float x, float low, float high)
{
	return x < low ? low : ((x > high) ? high : x);
}

static inline vg_color vg_color4f(float r, float g, float b, float a)
{
	uint8_t rb = (uint8_t)(vg_clampf(r, 0.0f, 1.0f) * 255.0f);
	uint8_t gb = (uint8_t)(vg_clampf(g, 0.0f, 1.0f) * 255.0f);
	uint8_t bb = (uint8_t)(vg_clampf(b, 0.0f, 1.0f) * 255.0f);
	uint8_t ab = (uint8_t)(vg_clampf(a, 0.0f, 1.0f) * 255.0f);
	return VG_COLOR32(rb, gb, bb, ab);
}

inline vg_color vg_color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	return VG_COLOR32(r, g, b, a);
}

inline vg_color vg_colorSetAlpha(vg_color c, uint8_t a)
{
	return (c & VG_COLOR_RGB_Msk) | (((uint32_t)a << VG_COLOR_ALPHA_Pos) & VG_COLOR_ALPHA_Msk);
}

inline uint8_t vg_colorGetAlpha(vg_color c)
{
	return (uint8_t)((c & VG_COLOR_ALPHA_Msk) >> VG_COLOR_ALPHA_Pos);
}

inline uint8_t vg_colorGetRed(vg_color c)
{
	return (uint8_t)((c & VG_COLOR_RED_Msk) >> VG_COLOR_RED_Pos);
}

inline uint8_t vg_colorGetGreen(vg_color c)
{
	return (uint8_t)((c & VG_COLOR_GREEN_Msk) >> VG_COLOR_GREEN_Pos);
}

inline uint8_t vg_colorGetBlue(vg_color c)
{
	return (uint8_t)((c & VG_COLOR_BLUE_Msk) >> VG_COLOR_BLUE_Pos);
}

