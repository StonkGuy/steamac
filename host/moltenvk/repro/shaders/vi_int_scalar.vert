#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in int col;
layout(location = 0) out vec4 v_col;
void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	gl_PointSize = 1.0;
	float r = intBitsToFloat(col);
	v_col = vec4(r, 0.5, 1.0 - r, 1.0);
}
