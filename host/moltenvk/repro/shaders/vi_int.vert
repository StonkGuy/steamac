#version 450
// STEAMAC-3N: float3/float4 vertex fetch into an int4 shader input. Preserve fetched bits.
layout(location = 0) in vec2 pos;
layout(location = 1) in ivec4 col;
layout(location = 0) out vec4 v_col;
void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	gl_PointSize = 1.0;
	v_col = intBitsToFloat(col);
}
