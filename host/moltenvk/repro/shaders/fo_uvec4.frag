#version 450
// uvec4 written to an R32_SINT attachment: the shape of STEAMAC-2C's error ("output of type uint4 is not
// compatible with a MTLPixelFormatR32Sint color attachment").
layout(location = 0) out uvec4 o;
void main()
{
	o = uvec4(0xfffffffeu, 7u, 8u, 9u);
}
