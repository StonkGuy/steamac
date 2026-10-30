#version 450
// provoking_last.c: rectangles of a 4x3 grid, every vertex with a color of its own (cell, vertex) as a flat
// varying; the cell is gl_VertexIndex / 8 + gl_InstanceIndex, the vertex gl_VertexIndex % 8 placed by topology.
layout(push_constant) uniform PC { uint topology; } pc; // 0 strip, 1 fan, 2 list
layout(location = 0) flat out uint color;

const vec2 strip[4] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 1));
const vec2 fan[4] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1));
const vec2 list[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));

void main()
{
	uint cell = uint(gl_VertexIndex) / 8u + uint(gl_InstanceIndex);
	uint k = uint(gl_VertexIndex) % 8u;
	vec2 p = pc.topology == 0u ? strip[k & 3u] : pc.topology == 1u ? fan[k & 3u] : list[min(k, 5u)];
	vec2 origin = vec2(float(cell % 4u) / 4.0, float(cell / 4u) / 3.0);
	vec2 uv = origin + (vec2(0.05) + p * 0.9) * vec2(0.25, 1.0 / 3.0);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
	color = 0xff000000u | (cell * 16u + k + 1u) << 8u | (pc.topology + 1u) * 40u;
}
