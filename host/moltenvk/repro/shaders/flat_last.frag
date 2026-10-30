#version 450
// provoking_last.c: the provoking vertex's color, bit exact into a B8G8R8A8_UNORM attachment
layout(location = 0) flat in uint color;
layout(location = 0) out vec4 o;

void main()
{
	o = unpackUnorm4x8(color).bgra;
}
