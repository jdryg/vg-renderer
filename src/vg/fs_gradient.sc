$input v_position

#include "../shaders/common.sh"

uniform mat3 u_paintMat;
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
	return min(max(d.x, d.y), 0.0) + length(max(d, 0.0) ) - rad;
}

void main()
{
	vec2 pt = mul(u_paintMat, vec3(v_position, 1.0)).xy;

	float d = clamp((sdroundrect(pt, u_extent, u_radius) + u_feather * 0.5) / u_feather, 0.0, 1.0);

	vec4 color = mix(u_innerCol, u_outerCol, d);

	gl_FragColor = color;
}
