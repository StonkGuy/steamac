#version 450
// Two uint outputs sharing location 0 (components 0 and 1) of an R32G32_SINT attachment.
layout(location = 0, component = 0) out uint a;
layout(location = 0, component = 1) out uint b;
void main()
{
	a = 0xfffffffdu;
	b = 5u;
}
