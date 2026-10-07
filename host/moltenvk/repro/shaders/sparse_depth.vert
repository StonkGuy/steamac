#version 460
// Full-screen triangle at depth p.depth for sparse_depth.frag (vkd3d-proton's test_sparse_depth_stencil_rendering)
layout(push_constant) uniform Params { float depth; float value; } p;
void main()
{
    vec2 xy = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(xy * 2.0 - 1.0, p.depth, 1.0);
}
