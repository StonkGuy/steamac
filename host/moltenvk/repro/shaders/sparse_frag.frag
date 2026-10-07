#version 460
// vkd3d-proton's texture_feedback Sample/SampleBias: residency of implicit-LOD samples in a fragment shader, one texel
// per pixel (LOD 0) plus a bias that may take the LOD past the last level.
#extension GL_ARB_sparse_texture2 : require
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Params { float bias; } p;
layout(location = 0) out vec4 color;
layout(location = 1) out uint resident;
void main()
{
    vec4 c;
    int code = sparseTextureARB(tex, gl_FragCoord.xy / 256.0, c, p.bias);
    color = c;
    resident = sparseTexelsResidentARB(code) ? 1u : 0u;
}
