#version 460
// Writes p.value where the depth test (GREATER) passes (vkd3d-proton's test_sparse_depth_stencil_rendering)
layout(push_constant) uniform Params { float depth; float value; } p;
layout(location = 0) out float color;
void main()
{
    color = p.value;
}
