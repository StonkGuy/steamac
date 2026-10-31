#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in uvec4 col;
layout(location = 0) out vec4 v_col;
void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	gl_PointSize = 1.0;
	float r = float(col.x & 255u) / 16.0;
	bool bits_ok = (col.x & 0x80000000u) != 0u && col.y == 0xffffffffu && col.z == 0x80000000u && col.w == 0x7fffffffu;
	v_col = bits_ok ? vec4(r, 0.5, 1.0 - r, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
