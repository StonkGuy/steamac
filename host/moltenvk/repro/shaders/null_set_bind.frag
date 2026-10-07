#version 450
// Counts fragment shader invocations through sets 0 and 4 of a five-set layout whose set 3 is bound as
// VK_NULL_HANDLE (Counter-Strike 2's binds, STEAMAC-25).
layout(set = 0, binding = 0, std430) buffer S0 { uint a; };
layout(set = 4, binding = 0, std430) buffer S4 { uint e; };
void main()
{
	atomicAdd(a, 1u);
	atomicAdd(e, 2u);
}
