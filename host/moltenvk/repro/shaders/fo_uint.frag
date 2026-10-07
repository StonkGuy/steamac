#version 450
// A scalar uint written to an R32_SINT attachment (padded to four components and converted).
layout(location = 0) out uint o;
void main()
{
	o = 0x80000001u;
}
