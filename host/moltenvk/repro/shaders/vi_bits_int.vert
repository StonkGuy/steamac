#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in ivec4 col;
layout(location = 0) out vec4 v_col;
void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	gl_PointSize = 1.0;
	float r = float(col.x & 255) / 16.0;
	bool bits_ok = col.x < 0 && col.y == -1 && col.z == (-2147483647 - 1) && col.w == 2147483647;
	v_col = bits_ok ? vec4(r, 0.5, 1.0 - r, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
