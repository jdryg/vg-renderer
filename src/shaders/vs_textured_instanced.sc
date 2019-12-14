$input a_position, a_color0, a_texcoord0, i_data0, i_data1
$output v_color0, v_texcoord0

#include "bgfx_shader.sh"

void main()
{
	mat4 modelToWorld = mat4(
		vec4(i_data0.x, i_data0.y, 0.0, 0.0),
		vec4(i_data0.z, i_data0.w, 0.0, 0.0),
		vec4(0.0, 0.0, 1.0, 0.0),
		vec4(i_data1.x, i_data1.y, 0.0, 1.0)
	);

	vec4 pos_model = vec4(a_position, 0.0, 1.0);
	vec4 pos_world = instMul(modelToWorld, pos_model);

	gl_Position = mul(u_viewProj, pos_world);
	v_color0 = a_color0;
	v_texcoord0 = a_texcoord0;
}
