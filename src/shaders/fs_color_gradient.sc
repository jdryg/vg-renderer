$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

uniform vec4 u_extentRadiusFeather;
uniform vec4 u_innerCol;
uniform vec4 u_outerCol;

#define u_extent   (u_extentRadiusFeather.xy)
#define u_radius   (u_extentRadiusFeather.z)
#define u_feather  (u_extentRadiusFeather.w)

float sdroundrect(vec2 pt, vec2 ext, float rad)
{
	vec2 ext2 = ext - vec2(rad, rad);
	vec2 d = abs(pt) - ext2;
	return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - rad;
}

void main()
{
	float d = clamp((sdroundrect(v_texcoord0, u_extent, u_radius) + u_feather * 0.5) / u_feather, 0.0, 1.0);
	vec4 gradient = mix(u_innerCol, u_outerCol, d);
	gl_FragColor = vec4(gradient.xyz, gradient.w * v_color0.w);
}
