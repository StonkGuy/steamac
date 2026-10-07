#version 450
// Location 0 float to RGBA8_UNORM (matching), location 1 ivec4 to R32G32B32A32_UINT.
layout(location = 0) out vec4 c;
layout(location = 1) out ivec4 i;
void main()
{
	c = vec4(1.0, 0.0, 0.0, 1.0);
	i = ivec4(-1, -2, 3, 4);
}
