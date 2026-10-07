#version 450
// Location 0 float to R32_UINT, location 1 ivec4 to RGBA8_UNORM: the values are bit cast.
layout(location = 0) out vec4 f;
layout(location = 1) out ivec4 i;
void main()
{
	f = vec4(1.0, 2.0, 3.0, 4.0);
	i = ivec4(floatBitsToInt(1.0), 0, floatBitsToInt(0.5), floatBitsToInt(1.0));
}
