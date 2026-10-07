#version 450
// An output array over two R32_SINT attachments, written through a dynamic index.
layout(location = 0) out uvec4 o[2];
layout(push_constant) uniform P { int n; } p;
void main()
{
	for (int k = 0; k < p.n; k++)
		o[k] = uvec4(0xfffffff0u + uint(k));
}
