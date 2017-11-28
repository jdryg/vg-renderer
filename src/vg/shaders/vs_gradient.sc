$input a_position
$output v_position

#include "../shaders/common.sh"

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 0.0, 1.0) );
	v_position = a_position;
}
