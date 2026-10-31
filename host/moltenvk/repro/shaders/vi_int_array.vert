#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in ivec2 col[2];
layout(location = 0) out vec4 v_col;
void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	gl_PointSize = 1.0;
	v_col = vec4(intBitsToFloat(col[gl_VertexIndex & 1]), intBitsToFloat(col[(gl_VertexIndex & 1) ^ 1]));
	// Exercise dynamic array indexing without changing the expected color.
	if ((gl_VertexIndex & 1) != 0)
		v_col = v_col.zwxy;
}
