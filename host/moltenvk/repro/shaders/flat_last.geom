#version 450
// provoking_last.c: a geometry shader between draws whose unrolls run before the render pass (its compute pass ends
// the render pass and allocates from the heap the indirect draws' unrolls use). Every vertex takes the triangle's
// largest color: its last vertex's in strips and lists, whatever order the input arrives in.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in uint vcolor[];
layout(location = 0) flat out uint color;

void main()
{
	uint c = max(max(vcolor[0], vcolor[1]), vcolor[2]);
	for (int i = 0; i < 3; i++) {
		gl_Position = gl_in[i].gl_Position;
		color = c;
		EmitVertex();
	}
}
