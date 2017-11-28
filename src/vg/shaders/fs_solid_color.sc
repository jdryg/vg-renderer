$input v_color0, v_texcoord0

/*
 * Copyright 2011-2016 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx#license-bsd-2-clause
 */

#include "../shaders/common.sh"

SAMPLER2D(s_tex, 0);

void main()
{
	vec4 texel = texture2D(s_tex, v_texcoord0.xy);
	gl_FragColor = v_color0 * texel; 
}
